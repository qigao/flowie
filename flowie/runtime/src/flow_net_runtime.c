#include "flow_net_runtime.h"

#include <cnet/websocket.h>
#include <salts/clock.h>
#include <salts/error_codes.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
#endif

typedef enum tf_net_command_kind {
  TF_NET_COMMAND_SEND = 1,
  TF_NET_COMMAND_CLOSE
} tf_net_command_kind;

typedef struct tf_net_command {
  tf_net_connection connection;
  size_t data_offset;
  size_t reserved_bytes;
  size_t size;
  int status;
  tf_net_command_kind kind;
} tf_net_command;

typedef struct tf_net_stream_peer {
  struct tf_net_server_impl *owner;
  cnet_connection connection;
  cnet_stream_peer peer;
  int close_status;
  bool used;
  bool opened;
  bool close_status_set;
} tf_net_stream_peer;

typedef struct tf_net_packet_peer {
  uint32_t generation;
  int close_status;
  bool opened;
  bool close_status_set;
} tf_net_packet_peer;

typedef struct tf_net_ws_peer {
  chttp_server_websocket_session session;
  tf_net_peer_info peer;
  uint32_t generation;
  int close_status;
  bool used;
  bool opened;
  bool close_status_set;
} tf_net_ws_peer;

typedef struct tf_net_server_impl {
  tf_net_server_config config;
  char *host;
  char *path;
  char *websocket_subprotocol;
  cnet_client stream;
  cnet_listener listener;
  cnet_tls_server tls;
  cnet_packet_endpoint packet;
  chttp_server websocket;
  tf_net_stream_peer *stream_peers;
  tf_net_packet_peer *packet_peers;
  tf_net_ws_peer *ws_peers;
  tf_net_command *commands;
  unsigned char *command_storage;
  size_t command_head;
  size_t command_count;
  size_t command_byte_head;
  size_t command_byte_tail;
  size_t command_bytes_used;
  salts_mutex_t mutex;
  salts_cond_t changed;
  salts_thread_t thread;
  uint16_t port;
  int terminal_status;
  bool sync_initialized;
  bool stream_initialized;
  bool listener_initialized;
  bool tls_initialized;
  bool packet_initialized;
  bool websocket_initialized;
  bool thread_started;
  bool started;
  bool stop_requested;
  bool worker_done;
} tf_net_server_impl;

static bool tf_net_power_of_two(size_t value) {
  return value != 0u && (value & (value - 1u)) == 0u;
}

int tf_net_peer_format(const tf_net_peer_info *peer, char *output, size_t capacity) {
  char address[INET6_ADDRSTRLEN] = {0};
  int written;
  if (output != NULL && capacity != 0u) output[0] = '\0';
  if (peer == NULL || output == NULL || capacity == 0u) return SALTS_EINVAL;
  if (peer->peer.family == CNET_DATAGRAM_ADDRESS_IPV4) {
    written = snprintf(address, sizeof(address), "%u.%u.%u.%u", peer->peer.address[0],
                       peer->peer.address[1], peer->peer.address[2], peer->peer.address[3]);
  } else if (peer->peer.family == CNET_DATAGRAM_ADDRESS_IPV6) {
#if defined(_WIN32)
    written = InetNtopA(AF_INET6, (void *)peer->peer.address, address, sizeof(address)) == NULL
                  ? -1
                  : (int)strlen(address);
#else
    written = inet_ntop(AF_INET6, peer->peer.address, address, sizeof(address)) == NULL
                  ? -1
                  : (int)strlen(address);
#endif
  } else {
    return SALTS_EINVAL;
  }
  if (written <= 0 || (size_t)written >= sizeof(address)) return SALTS_EIO;
  written = peer->peer.family == CNET_DATAGRAM_ADDRESS_IPV6
                ? snprintf(output, capacity, "[%s]:%u", address, (unsigned int)peer->peer.port)
                : snprintf(output, capacity, "%s:%u", address, (unsigned int)peer->peer.port);
  return written < 0 || (size_t)written >= capacity ? SALTS_ERANGE : SALTS_OK;
}

static bool tf_net_transport_valid(tf_net_transport transport) {
  return transport >= TF_NET_TRANSPORT_TCP && transport <= TF_NET_TRANSPORT_WSS;
}

static bool tf_net_transport_stream(tf_net_transport transport) {
  return transport == TF_NET_TRANSPORT_TCP || transport == TF_NET_TRANSPORT_TLS;
}

static bool tf_net_transport_packet(tf_net_transport transport) {
  return transport == TF_NET_TRANSPORT_UDP || transport == TF_NET_TRANSPORT_KCP;
}

static bool tf_net_transport_websocket(tf_net_transport transport) {
  return transport == TF_NET_TRANSPORT_WS || transport == TF_NET_TRANSPORT_WSS;
}

static char *tf_net_string_copy(const char *value) {
  char *copy;
  size_t size;
  if (value == NULL) return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy != NULL) memcpy(copy, value, size);
  return copy;
}

static bool tf_net_token_valid(const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  if (cursor == NULL || *cursor == 0u) return false;
  for (; *cursor != 0u; ++cursor) {
    const unsigned char ch = *cursor;
    if ((ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
        (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') ||
        (ch >= (unsigned char)'a' && ch <= (unsigned char)'z') ||
        strchr("!#$%&'*+-.^_`|~", (int)ch) != NULL)
      continue;
    return false;
  }
  return true;
}

static tf_net_connection tf_net_stream_handle(cnet_connection connection) {
  return (tf_net_connection){connection.slot, connection.generation};
}

static tf_net_connection tf_net_packet_handle(cnet_packet_session session) {
  return (tf_net_connection){session.slot, session.generation};
}

static bool tf_net_handle_valid(tf_net_connection connection) {
  return connection.slot != 0u && connection.generation != 0u;
}

static bool tf_net_handle_equal(tf_net_connection left, tf_net_connection right) {
  return left.slot == right.slot && left.generation == right.generation;
}

static tf_net_stream_peer *tf_net_stream_peer_find(tf_net_server_impl *server,
                                                    tf_net_connection connection) {
  size_t index;
  for (index = 0u; index < server->config.stream.connection_capacity; ++index) {
    tf_net_stream_peer *peer = &server->stream_peers[index];
    if (peer->used && peer->connection.slot == connection.slot &&
        peer->connection.generation == connection.generation)
      return peer;
  }
  return NULL;
}

static tf_net_stream_peer *tf_net_stream_peer_acquire(tf_net_server_impl *server) {
  size_t index;
  for (index = 0u; index < server->config.stream.connection_capacity; ++index) {
    tf_net_stream_peer *peer = &server->stream_peers[index];
    if (peer->used) continue;
    *peer = (tf_net_stream_peer){.owner = server, .used = true};
    return peer;
  }
  return NULL;
}

static tf_net_packet_peer *tf_net_packet_peer_find(tf_net_server_impl *server,
                                                    cnet_packet_session session) {
  tf_net_packet_peer *peer;
  const size_t index = (size_t)session.slot - 1u;
  if (session.slot == 0u || index >= server->config.stream.connection_capacity) return NULL;
  peer = &server->packet_peers[index];
  return peer->opened && peer->generation == session.generation ? peer : NULL;
}

static void tf_net_stream_state(void *user, cnet_connection connection,
                                cnet_connection_state state, const cnet_error *error) {
  tf_net_stream_peer *peer = (tf_net_stream_peer *)user;
  tf_net_server_impl *server;
  int status = error == NULL ? SALTS_OK : error->status;
  if (peer == NULL || (server = peer->owner) == NULL || !peer->used) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    tf_net_peer_info info = {.peer = peer->peer};
    if (server->config.transport == TF_NET_TRANSPORT_TLS) {
      const int certificate_status = cnet_tls_peer_certificate_sha256(
          &server->stream, connection, info.peer_certificate_sha256);
      if (certificate_status != SALTS_OK && certificate_status != SALTS_ENOENT) {
        (void)cnet_close(&server->stream, connection);
        return;
      }
    }
    peer->opened = true;
    status = server->config.observer.on_open(
        server->config.observer.user, tf_net_stream_handle(connection), &info);
    if (status == SALTS_OK) status = cnet_receive(&server->stream, connection, 1u);
    if (status != SALTS_OK) {
      peer->close_status = status;
      peer->close_status_set = true;
      (void)cnet_close(&server->stream, connection);
    }
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    const tf_net_connection handle = tf_net_stream_handle(connection);
    const bool opened = peer->opened;
    if (peer->close_status_set) status = peer->close_status;
    peer->used = false;
    peer->opened = false;
    peer->close_status_set = false;
    peer->connection = (cnet_connection){0};
    if (opened) server->config.observer.on_close(server->config.observer.user, handle, status);
  }
}

static void tf_net_stream_receive(void *user, cnet_connection connection,
                                  const cnet_receive_view *view) {
  tf_net_stream_peer *peer = (tf_net_stream_peer *)user;
  tf_net_server_impl *server;
  int status;
  if (peer == NULL || view == NULL || (server = peer->owner) == NULL || !peer->used) return;
  status = server->config.observer.on_receive(server->config.observer.user,
                                               tf_net_stream_handle(connection), view->data,
                                               view->size);
  if (status == SALTS_OK) status = cnet_receive(&server->stream, connection, 1u);
  if (status != SALTS_OK) {
    peer->close_status = status;
    peer->close_status_set = true;
    (void)cnet_close(&server->stream, connection);
  }
}

static void tf_net_stream_send(void *user, cnet_connection connection, size_t size) {
  tf_net_stream_peer *peer = (tf_net_stream_peer *)user;
  if (peer == NULL || peer->owner == NULL || !peer->used) return;
  if (peer->owner->config.observer.on_send != NULL)
    peer->owner->config.observer.on_send(peer->owner->config.observer.user,
                                         tf_net_stream_handle(connection), size);
}

static cnet_observer tf_net_stream_observer(tf_net_stream_peer *peer) {
  return (cnet_observer){.on_state = tf_net_stream_state,
                         .on_receive = tf_net_stream_receive,
                         .user = peer,
                         .on_send = tf_net_stream_send};
}

static int tf_net_packet_admit(void *user, cnet_packet_endpoint *endpoint,
                               cnet_packet_protocol protocol, const cnet_datagram_peer *peer,
                               uint32_t conversation) {
  tf_net_server_impl *server = (tf_net_server_impl *)user;
  (void)endpoint;
  (void)protocol;
  (void)peer;
  (void)conversation;
  return server == NULL || server->stop_requested ? SALTS_ESHUTDOWN : SALTS_OK;
}

static void tf_net_packet_state(void *user, cnet_packet_endpoint *endpoint,
                                cnet_packet_session session, cnet_packet_session_state state,
                                const cnet_datagram_peer *peer, uint32_t conversation) {
  tf_net_server_impl *server = (tf_net_server_impl *)user;
  tf_net_peer_info info = {0};
  int status;
  (void)endpoint;
  (void)conversation;
  if (server == NULL || peer == NULL) return;
  info.peer = (cnet_stream_peer){peer->family, peer->port, peer->scope_id, {0}};
  memcpy(info.peer.address, peer->address, sizeof(info.peer.address));
  if (state == CNET_PACKET_SESSION_OPEN) {
    tf_net_packet_peer *record;
    const size_t index = (size_t)session.slot - 1u;
    if (session.slot == 0u || index >= server->config.stream.connection_capacity) {
      (void)cnet_packet_session_close(&server->packet, session);
      return;
    }
    record = &server->packet_peers[index];
    *record = (tf_net_packet_peer){.generation = session.generation, .opened = true};
    status = server->config.observer.on_open(server->config.observer.user,
                                              tf_net_packet_handle(session), &info);
    if (status != SALTS_OK) {
      record->close_status = status;
      record->close_status_set = true;
      (void)cnet_packet_session_close(&server->packet, session);
    }
  } else if (state == CNET_PACKET_SESSION_CLOSED) {
    tf_net_packet_peer *record = tf_net_packet_peer_find(server, session);
    if (record != NULL) {
      const int close_status = record->close_status_set ? record->close_status : SALTS_OK;
      *record = (tf_net_packet_peer){0};
      server->config.observer.on_close(server->config.observer.user,
                                       tf_net_packet_handle(session), close_status);
    }
  }
}

static void tf_net_packet_receive(void *user, cnet_packet_endpoint *endpoint,
                                  cnet_packet_session session, const cnet_receive_view *view) {
  tf_net_server_impl *server = (tf_net_server_impl *)user;
  int status;
  (void)endpoint;
  if (server == NULL || view == NULL) return;
  status = server->config.observer.on_receive(server->config.observer.user,
                                               tf_net_packet_handle(session), view->data,
                                               view->size);
  if (status != SALTS_OK) {
    tf_net_packet_peer *peer = tf_net_packet_peer_find(server, session);
    if (peer != NULL) {
      peer->close_status = status;
      peer->close_status_set = true;
    }
    (void)cnet_packet_session_close(&server->packet, session);
  }
}

static void tf_net_packet_error(void *user, cnet_packet_endpoint *endpoint,
                                cnet_packet_session session, int status) {
  tf_net_server_impl *server = (tf_net_server_impl *)user;
  (void)endpoint;
  if (server != NULL) {
    tf_net_packet_peer *peer = tf_net_packet_peer_find(server, session);
    if (peer != NULL) {
      if (!peer->close_status_set) {
        peer->close_status = status;
        peer->close_status_set = true;
      }
      (void)cnet_packet_session_close(&server->packet, session);
    }
  }
}

static tf_net_ws_peer *tf_net_ws_peer_find_session(tf_net_server_impl *server,
                                                    chttp_server_websocket_session session) {
  size_t index;
  for (index = 0u; index < server->config.stream.connection_capacity; ++index) {
    tf_net_ws_peer *peer = &server->ws_peers[index];
    if (peer->used && peer->session.impl == session.impl &&
        peer->session.connection_slot == session.connection_slot &&
        peer->session.connection_generation == session.connection_generation &&
        peer->session.stream_id == session.stream_id)
      return peer;
  }
  return NULL;
}

static int tf_net_ws_open(void *user, chttp_websocket *websocket,
                          const chttp_server_request_view *request,
                          chttp_server_response *response) {
  tf_net_server_impl *server = (tf_net_server_impl *)user;
  chttp_server_websocket_session session = {0};
  tf_net_ws_peer *peer = NULL;
  size_t index;
  int status;
  if (server == NULL || request == NULL || request->peer == NULL) return SALTS_EINVAL;
  if (server->websocket_subprotocol != NULL) {
    status = chttp_server_response_select_websocket_subprotocol(
        response, request, server->websocket_subprotocol);
    if (status == SALTS_EPROTO || status == SALTS_EINVAL)
      return chttp_server_reply(response, 400u, NULL, NULL, 0u);
    if (status != SALTS_OK) return status;
  }
  status = chttp_server_websocket_session_capture(websocket, &session);
  if (status != SALTS_OK) return status;
  salts_mutex_lock(&server->mutex);
  for (index = 0u; index < server->config.stream.connection_capacity; ++index) {
    if (server->ws_peers[index].used) continue;
    peer = &server->ws_peers[index];
    ++peer->generation;
    if (peer->generation == 0u) ++peer->generation;
    peer->used = true;
    peer->opened = true;
    peer->close_status_set = false;
    peer->session = session;
    peer->peer.peer = *request->peer;
    if (request->peer_certificate_sha256 != NULL)
      memcpy(peer->peer.peer_certificate_sha256, request->peer_certificate_sha256,
             CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY);
    break;
  }
  salts_mutex_unlock(&server->mutex);
  if (peer == NULL) return SALTS_ENOBUFS;
  status = server->config.observer.on_open(
      server->config.observer.user, (tf_net_connection){(uint32_t)(index + 1u), peer->generation},
      &peer->peer);
  if (status != SALTS_OK) {
    salts_mutex_lock(&server->mutex);
    peer->used = false;
    peer->opened = false;
    salts_mutex_unlock(&server->mutex);
  }
  return status;
}

static void tf_net_ws_event(void *user, chttp_websocket *websocket,
                            const chttp_websocket_event *event) {
  tf_net_server_impl *server = (tf_net_server_impl *)user;
  chttp_server_websocket_session session = {0};
  tf_net_connection handle;
  bool opened = false;
  bool close_status_set = false;
  int close_status = SALTS_OK;
  int status = SALTS_OK;
  if (server == NULL || event == NULL ||
      chttp_server_websocket_session_capture(websocket, &session) != SALTS_OK)
    return;
  salts_mutex_lock(&server->mutex);
  {
    tf_net_ws_peer *peer = tf_net_ws_peer_find_session(server, session);
    if (peer != NULL) {
    handle = (tf_net_connection){(uint32_t)(peer - server->ws_peers + 1u), peer->generation};
      opened = peer->opened;
      close_status_set = peer->close_status_set;
      close_status = peer->close_status;
      if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
        peer->used = false;
        peer->opened = false;
        peer->close_status_set = false;
        peer->session = (chttp_server_websocket_session){0};
      }
    } else {
      handle = (tf_net_connection){0};
    }
  }
  salts_mutex_unlock(&server->mutex);
  if (!tf_net_handle_valid(handle)) return;
  if (event->kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
    status = event->message_type == CHTTP_WEBSOCKET_MESSAGE_BINARY
                 ? server->config.observer.on_receive(server->config.observer.user, handle,
                                                       event->data, event->size)
                 : SALTS_EPROTO;
    if (status != SALTS_OK) {
      const size_t index = (size_t)handle.slot - 1u;
      salts_mutex_lock(&server->mutex);
      if (index < server->config.stream.connection_capacity && server->ws_peers[index].used &&
          server->ws_peers[index].generation == handle.generation) {
        server->ws_peers[index].close_status = status;
        server->ws_peers[index].close_status_set = true;
      }
      salts_mutex_unlock(&server->mutex);
      (void)chttp_websocket_close(websocket, 1002u, NULL, 0u);
    }
  } else if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
    if (opened)
      server->config.observer.on_close(server->config.observer.user, handle,
                                       close_status_set ? close_status : SALTS_OK);
  }
}

static int tf_net_command_progress(tf_net_server_impl *server) {
  for (;;) {
    tf_net_command command;
    const unsigned char *data;
    int status;
    salts_mutex_lock(&server->mutex);
    if (server->command_count == 0u) {
      salts_mutex_unlock(&server->mutex);
      return SALTS_OK;
    }
    command = server->commands[server->command_head];
    data = server->command_storage + command.data_offset;
    salts_mutex_unlock(&server->mutex);
    if (tf_net_transport_stream(server->config.transport)) {
      tf_net_stream_peer *peer = tf_net_stream_peer_find(server, command.connection);
      if (peer == NULL)
        status = SALTS_ENOENT;
      else if (command.kind == TF_NET_COMMAND_SEND)
        status = cnet_send(&server->stream, peer->connection, data, command.size);
      else {
        peer->close_status = command.status;
        peer->close_status_set = true;
        status = cnet_close(&server->stream, peer->connection);
      }
    } else if (command.kind == TF_NET_COMMAND_SEND) {
      status = cnet_packet_send(&server->packet,
                                (cnet_packet_session){command.connection.slot,
                                                      command.connection.generation},
                                data, command.size);
    } else {
      cnet_packet_session session = {command.connection.slot, command.connection.generation};
      tf_net_packet_peer *peer = tf_net_packet_peer_find(server, session);
      if (peer != NULL) {
        peer->close_status = command.status;
        peer->close_status_set = true;
      }
      status = cnet_packet_session_close(
          &server->packet, session);
    }
    if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) return SALTS_OK;
    salts_mutex_lock(&server->mutex);
    server->commands[server->command_head] = (tf_net_command){0};
    server->command_head = (server->command_head + 1u) % server->config.command_capacity;
    --server->command_count;
    server->command_byte_head =
        (server->command_byte_head + command.reserved_bytes) % server->config.command_bytes_capacity;
    server->command_bytes_used -= command.reserved_bytes;
    if (server->command_count == 0u) {
      server->command_byte_head = 0u;
      server->command_byte_tail = 0u;
    }
    salts_mutex_unlock(&server->mutex);
  }
}

static int tf_net_stream_accept(tf_net_server_impl *server) {
  for (;;) {
    tf_net_stream_peer *peer = tf_net_stream_peer_acquire(server);
    cnet_observer observer;
    cnet_connection connection = {0};
    cnet_stream_peer address = {0};
    int status;
    if (peer == NULL) return SALTS_OK;
    observer = tf_net_stream_observer(peer);
    status = server->config.transport == TF_NET_TRANSPORT_TLS
                 ? cnet_listener_accept_tls_peer(&server->listener, &server->stream, &server->tls,
                                                 &observer, &connection, &address)
                 : cnet_listener_accept_peer(&server->listener, &server->stream, &observer,
                                             &connection, &address);
    if (status == SALTS_ETIMEDOUT) {
      peer->used = false;
      return SALTS_OK;
    }
    if (status != SALTS_OK) {
      peer->used = false;
      return status;
    }
    peer->connection = connection;
    peer->peer = address;
  }
}

static bool tf_net_should_stop(tf_net_server_impl *server) {
  bool stop;
  salts_mutex_lock(&server->mutex);
  stop = server->stop_requested;
  salts_mutex_unlock(&server->mutex);
  return stop;
}

static void tf_net_worker_finish(tf_net_server_impl *server, int status) {
  salts_mutex_lock(&server->mutex);
  server->terminal_status = status;
  server->worker_done = true;
  salts_cond_broadcast(&server->changed);
  salts_mutex_unlock(&server->mutex);
}

static void tf_net_worker(void *user) {
  tf_net_server_impl *server = (tf_net_server_impl *)user;
  int status = SALTS_OK;
  while (!tf_net_should_stop(server)) {
    size_t events = 0u;
    status = tf_net_command_progress(server);
    if (status != SALTS_OK) break;
    if (tf_net_transport_stream(server->config.transport)) {
      int ready = 0;
      status = cnet_listener_wait(&server->listener, 0u, &ready);
      if (status == SALTS_OK && ready) status = tf_net_stream_accept(server);
      if (status == SALTS_OK)
        status = cnet_client_poll(&server->stream, server->config.poll_slice_ms, &events);
    } else {
      status = cnet_packet_poll(&server->packet, server->config.poll_slice_ms, &events);
    }
    if (status != SALTS_OK) break;
  }
  if (tf_net_transport_stream(server->config.transport)) {
    int stop_status;
    if (server->listener_initialized) (void)cnet_listener_close(&server->listener);
    do {
      stop_status = cnet_client_stop(&server->stream, server->config.poll_slice_ms);
    } while (stop_status == SALTS_ETIMEDOUT);
    if (status == SALTS_OK && stop_status != SALTS_OK && stop_status != SALTS_EALREADY)
      status = stop_status;
  } else {
    int stop_status;
    do {
      stop_status = cnet_packet_endpoint_stop(&server->packet, server->config.poll_slice_ms);
    } while (stop_status == SALTS_ETIMEDOUT);
    if (status == SALTS_OK && stop_status != SALTS_OK && stop_status != SALTS_EALREADY)
      status = stop_status;
  }
  tf_net_worker_finish(server, status);
}

static void tf_net_impl_free(tf_net_server_impl *server) {
  if (server == NULL) return;
  if (server->listener_initialized) (void)cnet_listener_close(&server->listener);
  if (server->packet_initialized) (void)cnet_packet_endpoint_stop(&server->packet, 0u);
  if (server->stream_initialized) (void)cnet_client_stop(&server->stream, 0u);
  if (server->websocket_initialized) (void)chttp_server_destroy(&server->websocket);
  if (server->packet_initialized) (void)cnet_packet_endpoint_destroy(&server->packet);
  if (server->stream_initialized) (void)cnet_client_destroy(&server->stream);
  if (server->listener_initialized) (void)cnet_listener_destroy(&server->listener);
  if (server->tls_initialized) (void)cnet_tls_server_destroy(&server->tls);
  if (server->sync_initialized) {
    salts_cond_destroy(&server->changed);
    salts_mutex_destroy(&server->mutex);
  }
  free(server->command_storage);
  free(server->commands);
  free(server->ws_peers);
  free(server->packet_peers);
  free(server->stream_peers);
  free(server->websocket_subprotocol);
  free(server->path);
  free(server->host);
  free(server);
}

static int tf_net_init_stream(tf_net_server_impl *server) {
  cnet_listener_config listener = {.backend = server->config.stream.backend,
                                   .host = server->host,
                                   .port = server->config.port,
                                   .backlog = server->config.backlog};
  int status = cnet_client_init(&server->stream, &server->config.stream);
  if (status != SALTS_OK) return status;
  server->stream_initialized = true;
  status = cnet_client_set_stream_socket_options(&server->stream,
                                                 &server->config.stream_socket_options);
  if (status != SALTS_OK) return status;
  if (server->config.transport == TF_NET_TRANSPORT_TLS) {
    status = cnet_tls_server_init(&server->tls, server->config.tls);
    if (status != SALTS_OK) return status;
    server->tls_initialized = true;
  }
  status = cnet_listener_init_ex(&server->listener, &listener,
                                 &server->config.listener_options);
  if (status != SALTS_OK) return status;
  server->listener_initialized = true;
  return cnet_listener_port(&server->listener, &server->port);
}

static int tf_net_init_packet(tf_net_server_impl *server) {
  cnet_packet_endpoint_config packet = server->config.packet;
  int status;
  packet.protocol = server->config.transport == TF_NET_TRANSPORT_UDP ? CNET_PACKET_UDP
                                                                     : CNET_PACKET_KCP;
  packet.session_capacity = server->config.stream.connection_capacity;
  packet.datagram.host = server->host;
  packet.datagram.port = server->config.port;
  packet.observer = (cnet_packet_observer){.on_admit = tf_net_packet_admit,
                                           .on_state = tf_net_packet_state,
                                           .on_receive = tf_net_packet_receive,
                                           .on_error = tf_net_packet_error,
                                           .user = server};
  packet.datagram.observer = (cnet_datagram_observer){0};
  packet.kcp.observer = (cnet_kcp_observer){0};
  server->config.packet = packet;
  status = cnet_packet_endpoint_init(&server->packet, &packet);
  if (status != SALTS_OK) return status;
  server->packet_initialized = true;
  return cnet_packet_endpoint_port(&server->packet, &server->port);
}

static int tf_net_init_websocket(tf_net_server_impl *server) {
  chttp_server_config config = {.host = server->host,
                                .port = server->config.port,
                                .backlog = server->config.backlog,
                                .network = server->config.stream,
                                .route_capacity = 1u,
                                .max_target_bytes = 1024u,
                                .max_header_count = 32u,
                                .max_header_bytes = 8192u,
                                .max_request_body_bytes = 1u,
                                .max_response_header_count = 8u,
                                .max_response_header_bytes = 1024u,
                                .max_response_body_bytes = 1u,
                                .poll_slice_ms = server->config.poll_slice_ms,
                                .tls = server->config.transport == TF_NET_TRANSPORT_WSS
                                           ? server->config.tls
                                           : NULL};
  chttp_server_websocket_options route = {.size = sizeof(route),
                                          .path = server->path,
                                          .max_frame_bytes = server->config.max_message_bytes,
                                          .max_message_bytes = server->config.max_message_bytes,
                                          .on_open = tf_net_ws_open,
                                          .on_event = tf_net_ws_event,
                                          .user = server};
  chttp_server_socket_options socket_options = {
      .size = sizeof(socket_options),
      .stream = server->config.stream_socket_options,
      .listener = server->config.listener_options};
  int status = chttp_server_init(&server->websocket, &config);
  if (status != SALTS_OK) return status;
  server->websocket_initialized = true;
  status = chttp_server_set_socket_options(&server->websocket, &socket_options);
  if (status != SALTS_OK) return status;
  return chttp_server_websocket_with(&server->websocket, &route);
}

int tf_net_server_init(tf_net_server *server, const tf_net_server_config *config) {
  tf_net_server_impl *impl;
  int status;
  if (server == NULL || config == NULL || server->impl != NULL ||
      config->size != sizeof(*config) || !tf_net_transport_valid(config->transport) ||
      config->host == NULL || config->host[0] == '\0' || config->backlog == 0u ||
      config->stream.connection_capacity == 0u || !tf_net_power_of_two(config->command_capacity) ||
      config->command_bytes_capacity == 0u || config->max_message_bytes == 0u ||
      config->command_bytes_capacity < config->max_message_bytes || config->poll_slice_ms == 0u ||
      config->observer.on_open == NULL || config->observer.on_receive == NULL ||
      config->observer.on_close == NULL ||
      (tf_net_transport_websocket(config->transport) &&
       (config->path == NULL || config->path[0] != '/' ||
        (config->websocket_subprotocol != NULL &&
         !tf_net_token_valid(config->websocket_subprotocol)))) ||
      ((config->transport == TF_NET_TRANSPORT_TLS || config->transport == TF_NET_TRANSPORT_WSS) &&
       config->tls == NULL))
    return SALTS_EINVAL;
  if (tf_net_transport_websocket(config->transport) &&
      (config->max_message_bytes > SIZE_MAX - CNET_WEBSOCKET_MAX_HEADER_BYTES ||
       config->stream.max_send_bytes <
           config->max_message_bytes + CNET_WEBSOCKET_MAX_HEADER_BYTES))
    return SALTS_EMSGSIZE;
  if (config->stream_socket_options.size != 0u) {
    status = cnet_stream_socket_options_validate(&config->stream_socket_options);
    if (status != SALTS_OK) return status;
  }
  if (config->listener_options.size != 0u) {
    status = cnet_listener_options_validate(&config->listener_options);
    if (status != SALTS_OK) return status;
  }
  impl = (tf_net_server_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->config = *config;
  if (impl->config.stream_socket_options.size == 0u)
    impl->config.stream_socket_options =
        (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
  if (impl->config.listener_options.size == 0u)
    impl->config.listener_options = (cnet_listener_options)CNET_LISTENER_OPTIONS_INIT;
  impl->host = tf_net_string_copy(config->host);
  impl->path = tf_net_string_copy(config->path == NULL ? "/" : config->path);
  impl->websocket_subprotocol = tf_net_string_copy(config->websocket_subprotocol);
  impl->commands = (tf_net_command *)calloc(config->command_capacity, sizeof(*impl->commands));
  impl->command_storage = (unsigned char *)malloc(config->command_bytes_capacity);
  impl->stream_peers = (tf_net_stream_peer *)calloc(config->stream.connection_capacity,
                                                     sizeof(*impl->stream_peers));
  impl->packet_peers = (tf_net_packet_peer *)calloc(config->stream.connection_capacity,
                                                     sizeof(*impl->packet_peers));
  impl->ws_peers =
      (tf_net_ws_peer *)calloc(config->stream.connection_capacity, sizeof(*impl->ws_peers));
  if (impl->host == NULL || impl->path == NULL ||
      (config->websocket_subprotocol != NULL && impl->websocket_subprotocol == NULL) ||
      impl->commands == NULL ||
      impl->command_storage == NULL || impl->stream_peers == NULL || impl->packet_peers == NULL ||
      impl->ws_peers == NULL) {
    tf_net_impl_free(impl);
    return SALTS_ENOMEM;
  }
  salts_mutex_init(&impl->mutex);
  salts_cond_init(&impl->changed);
  impl->sync_initialized = true;
  impl->config.host = impl->host;
  impl->config.path = impl->path;
  impl->config.websocket_subprotocol = impl->websocket_subprotocol;
  if (tf_net_transport_stream(config->transport))
    status = tf_net_init_stream(impl);
  else if (tf_net_transport_packet(config->transport))
    status = tf_net_init_packet(impl);
  else
    status = tf_net_init_websocket(impl);
  if (status != SALTS_OK) {
    tf_net_impl_free(impl);
    return status;
  }
  server->impl = impl;
  return SALTS_OK;
}

int tf_net_server_start(tf_net_server *server) {
  tf_net_server_impl *impl;
  int status;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (tf_net_server_impl *)server->impl;
  if (impl->started) return SALTS_EALREADY;
  if (tf_net_transport_websocket(impl->config.transport)) {
    status = chttp_server_start(&impl->websocket);
    if (status == SALTS_OK) status = chttp_server_port(&impl->websocket, &impl->port);
  } else {
    status = salts_thread_create(&impl->thread, tf_net_worker, impl);
    if (status == SALTS_OK) impl->thread_started = true;
    else status = SALTS_EIO;
  }
  if (status == SALTS_OK) impl->started = true;
  return status;
}

int tf_net_server_port(const tf_net_server *server, uint16_t *out_port) {
  const tf_net_server_impl *impl;
  if (server == NULL || server->impl == NULL || out_port == NULL) return SALTS_EINVAL;
  impl = (const tf_net_server_impl *)server->impl;
  *out_port = impl->port;
  return SALTS_OK;
}

static int tf_net_command_submit(tf_net_server_impl *server, tf_net_connection connection,
                                 tf_net_command_kind kind, const void *data, size_t size,
                                 int close_status) {
  tf_net_command *command;
  unsigned char *storage;
  size_t data_offset;
  size_t reserved_bytes;
  size_t tail;
  if (!tf_net_handle_valid(connection) || (data == NULL && size != 0u) ||
      size > server->config.max_message_bytes)
    return SALTS_EINVAL;
  salts_mutex_lock(&server->mutex);
  if (!server->started || server->stop_requested || server->worker_done) {
    salts_mutex_unlock(&server->mutex);
    return SALTS_ESHUTDOWN;
  }
  if (server->command_count == server->config.command_capacity) {
    salts_mutex_unlock(&server->mutex);
    return SALTS_ENOBUFS;
  }
  if (size > server->config.command_bytes_capacity - server->command_bytes_used) {
    salts_mutex_unlock(&server->mutex);
    return SALTS_ENOBUFS;
  }
  data_offset = server->command_byte_tail;
  reserved_bytes = size;
  if (size != 0u && server->command_byte_tail >= server->command_byte_head &&
      size > server->config.command_bytes_capacity - server->command_byte_tail) {
    const size_t padding = server->config.command_bytes_capacity - server->command_byte_tail;
    if (size > server->command_byte_head || padding >
                                                server->config.command_bytes_capacity -
                                                    server->command_bytes_used - size) {
      salts_mutex_unlock(&server->mutex);
      return SALTS_ENOBUFS;
    }
    data_offset = 0u;
    reserved_bytes += padding;
  } else if (size != 0u && server->command_byte_tail < server->command_byte_head &&
             size > server->command_byte_head - server->command_byte_tail) {
    salts_mutex_unlock(&server->mutex);
    return SALTS_ENOBUFS;
  }
  tail = (server->command_head + server->command_count) % server->config.command_capacity;
  command = &server->commands[tail];
  storage = server->command_storage + data_offset;
  if (size != 0u) memcpy(storage, data, size);
  *command = (tf_net_command){.connection = connection,
                              .data_offset = data_offset,
                              .reserved_bytes = reserved_bytes,
                              .size = size,
                              .status = close_status,
                              .kind = kind};
  ++server->command_count;
  server->command_byte_tail =
      (server->command_byte_tail + reserved_bytes) % server->config.command_bytes_capacity;
  server->command_bytes_used += reserved_bytes;
  salts_mutex_unlock(&server->mutex);
  if (tf_net_transport_stream(server->config.transport))
    (void)cnet_client_wake(&server->stream);
  else
    (void)cnet_packet_wake(&server->packet);
  return SALTS_OK;
}

static bool tf_net_ws_session_copy(tf_net_server_impl *server, tf_net_connection connection,
                                   chttp_server_websocket_session *out_session) {
  tf_net_ws_peer *peer;
  const size_t index = (size_t)connection.slot - 1u;
  bool found;
  if (connection.slot == 0u || index >= server->config.stream.connection_capacity) return false;
  salts_mutex_lock(&server->mutex);
  peer = &server->ws_peers[index];
  if (!peer->used || peer->generation != connection.generation) peer = NULL;
  if (peer != NULL) *out_session = peer->session;
  found = peer != NULL;
  salts_mutex_unlock(&server->mutex);
  return found;
}

int tf_net_server_send(tf_net_server *server, tf_net_connection connection, const void *data,
                       size_t size) {
  tf_net_server_impl *impl;
  if (server == NULL || server->impl == NULL || data == NULL || size == 0u)
    return SALTS_EINVAL;
  impl = (tf_net_server_impl *)server->impl;
  if (size > impl->config.max_message_bytes) return SALTS_EMSGSIZE;
  if (tf_net_transport_websocket(impl->config.transport)) {
    chttp_server_websocket_session session;
    if (!tf_net_ws_session_copy(impl, connection, &session)) return SALTS_ENOENT;
    return chttp_server_websocket_send_binary(&session, data, size);
  }
  return tf_net_command_submit(impl, connection, TF_NET_COMMAND_SEND, data, size, SALTS_OK);
}

int tf_net_server_close(tf_net_server *server, tf_net_connection connection, int status) {
  tf_net_server_impl *impl;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (tf_net_server_impl *)server->impl;
  if (tf_net_transport_websocket(impl->config.transport)) {
    chttp_server_websocket_session session;
    const size_t index = (size_t)connection.slot - 1u;
    int close_result;
    if (!tf_net_ws_session_copy(impl, connection, &session)) return SALTS_ENOENT;
    salts_mutex_lock(&impl->mutex);
    if (index < impl->config.stream.connection_capacity && impl->ws_peers[index].used &&
        impl->ws_peers[index].generation == connection.generation) {
      impl->ws_peers[index].close_status = status;
      impl->ws_peers[index].close_status_set = true;
    }
    salts_mutex_unlock(&impl->mutex);
    close_result = chttp_server_websocket_close(&session, 1000u, NULL, 0u);
    if (close_result != SALTS_OK) {
      salts_mutex_lock(&impl->mutex);
      if (index < impl->config.stream.connection_capacity && impl->ws_peers[index].used &&
          impl->ws_peers[index].generation == connection.generation &&
          impl->ws_peers[index].close_status_set &&
          impl->ws_peers[index].close_status == status)
        impl->ws_peers[index].close_status_set = false;
      salts_mutex_unlock(&impl->mutex);
    }
    return close_result;
  }
  return tf_net_command_submit(impl, connection, TF_NET_COMMAND_CLOSE, NULL, 0u, status);
}

int tf_net_server_stop(tf_net_server *server, uint32_t timeout_ms) {
  tf_net_server_impl *impl;
  uint64_t started_ms;
  int status;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (tf_net_server_impl *)server->impl;
  if (!impl->started) return SALTS_OK;
  if (tf_net_transport_websocket(impl->config.transport)) {
    status = chttp_server_stop(&impl->websocket, timeout_ms);
    if (status != SALTS_ETIMEDOUT && status != SALTS_EBUSY) impl->started = false;
    return status;
  }
  started_ms = salts_monotonic_ms();
  salts_mutex_lock(&impl->mutex);
  impl->stop_requested = true;
  salts_mutex_unlock(&impl->mutex);
  if (tf_net_transport_stream(impl->config.transport))
    (void)cnet_client_wake(&impl->stream);
  else
    (void)cnet_packet_wake(&impl->packet);
  salts_mutex_lock(&impl->mutex);
  while (!impl->worker_done) {
    const uint64_t elapsed = salts_monotonic_ms() - started_ms;
    if (timeout_ms != 0u && elapsed >= timeout_ms) {
      salts_mutex_unlock(&impl->mutex);
      return SALTS_ETIMEDOUT;
    }
    if (timeout_ms == 0u)
      salts_cond_wait(&impl->changed, &impl->mutex);
    else if (salts_cond_timedwait(&impl->changed, &impl->mutex,
                                  ((uint64_t)timeout_ms - elapsed) * 1000000u) != SALTS_OK &&
             !impl->worker_done) {
      salts_mutex_unlock(&impl->mutex);
      return SALTS_ETIMEDOUT;
    }
  }
  status = impl->terminal_status;
  salts_mutex_unlock(&impl->mutex);
  if (impl->thread_started) {
    if (salts_thread_join(&impl->thread) != SALTS_OK) return SALTS_EIO;
    salts_thread_destroy(&impl->thread);
    impl->thread_started = false;
  }
  impl->started = false;
  return status;
}

int tf_net_server_destroy(tf_net_server *server) {
  tf_net_server_impl *impl;
  if (server == NULL) return SALTS_EINVAL;
  if (server->impl == NULL) return SALTS_OK;
  impl = (tf_net_server_impl *)server->impl;
  if (impl->started || impl->thread_started) return SALTS_EBUSY;
  tf_net_impl_free(impl);
  server->impl = NULL;
  return SALTS_OK;
}
