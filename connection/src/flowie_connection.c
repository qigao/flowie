#include "flowie_connection.h"
#include "flowie_send_batch.h"
#include "flowie_affinity.h"

#include <cnet/websocket.h>
#include <cnet/manager.h>
#include <cnet/handoff.h>
#include <cnet/owner_placement.h>
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

typedef enum flowie_command_kind {
  TF_NET_COMMAND_SEND = 1,
  TF_NET_COMMAND_SEND_SLICES,
  TF_NET_COMMAND_CLOSE
} flowie_command_kind;

typedef struct flowie_command {
  flowie_connection connection;
  size_t data_offset;
  size_t reserved_bytes;
  size_t size;
  mem_slice_t *slices;
  size_t slice_count;
  int status;
  flowie_command_kind kind;
} flowie_command;

typedef struct flowie_stream_peer {
  struct flowie_server_impl *owner;
  cnet_connection connection;
  cnet_managed_connection managed;
  cnet_handoff_ticket ticket;
  cnet_stream_peer peer;
  int close_status;
  bool used;
  bool opened;
  bool close_status_set;
} flowie_stream_peer;

typedef struct flowie_packet_peer {
  uint32_t generation;
  int close_status;
  bool opened;
  bool close_status_set;
} flowie_packet_peer;

typedef struct flowie_ws_peer {
  chttp_server_websocket_session session;
  flowie_peer_info peer;
  uint32_t generation;
  int close_status;
  bool used;
  bool opened;
  bool close_status_set;
} flowie_ws_peer;

typedef struct flowie_server_impl {
  flowie_server_config config;
  char *host;
  char *path;
  char *websocket_subprotocol;
  cnet_client stream;
  cnet_manager manager;
  cnet_handoff handoff;
  int management_status;
  cnet_listener listener;
  cnet_tls_server tls;
  cnet_packet_endpoint packet;
  chttp_server websocket;
  flowie_stream_peer *stream_peers;
  flowie_packet_peer *packet_peers;
  flowie_ws_peer *ws_peers;
  flowie_command *commands;
  unsigned char *command_storage;
  size_t command_head;
  size_t command_count;
  size_t command_byte_head;
  size_t command_byte_tail;
  size_t command_bytes_used;
  size_t command_payload_bytes_used;
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  cmeta_thread_t thread;
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
  bool worker_ready;
  int startup_status;
  /* Root owns the listener and this fixed array; each entry owns one CNet client. */
  struct flowie_server_impl *root;
  struct flowie_server_impl **owners;
  uint32_t owner_count;
  uint32_t owner_index;
  cmeta_thread_t accept_thread;
  bool accept_thread_started;
  bool accept_done;
} flowie_server_impl;

static bool flowie_power_of_two(size_t value) {
  return value != 0u && (value & (value - 1u)) == 0u;
}

static void flowie_command_slices_release(flowie_command *command) {
  size_t index;
  if (command == NULL || command->slices == NULL) return;
  for (index = 0u; index < command->slice_count; ++index)
    mem_slice_release(&command->slices[index]);
  free(command->slices);
  command->slices = NULL;
  command->slice_count = 0u;
}

static int flowie_command_slice_clone(const mem_slice_t *source, mem_slice_t *out) {
  const char *base;
  uintptr_t base_address;
  uintptr_t data_address;
  size_t used;
  size_t offset;
  if (source == NULL || out == NULL || source->buffer == NULL || source->data == NULL ||
      source->length == 0u)
    return SALTS_EINVAL;
  base = mem_buffer_const_data(source->buffer);
  used = mem_buffer_used(source->buffer);
  if (base == NULL || used == 0u) return SALTS_EINVAL;
  base_address = (uintptr_t)base;
  data_address = (uintptr_t)source->data;
  if (data_address < base_address) return SALTS_EINVAL;
  offset = (size_t)(data_address - base_address);
  if (offset > used || source->length > used - offset) return SALTS_EINVAL;
  *out = mem_slice(source->buffer, offset, source->length);
  return out->buffer != NULL ? SALTS_OK : SALTS_ENOMEM;
}

int flowie_peer_format(const flowie_peer_info *peer, char *output, size_t capacity) {
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

static bool flowie_transport_valid(flowie_transport transport) {
  return transport >= TF_NET_TRANSPORT_TCP && transport <= TF_NET_TRANSPORT_WSS;
}

static bool flowie_transport_stream(flowie_transport transport) {
  return transport == TF_NET_TRANSPORT_TCP || transport == TF_NET_TRANSPORT_TLS;
}

static bool flowie_transport_packet(flowie_transport transport) {
  return transport == TF_NET_TRANSPORT_UDP || transport == TF_NET_TRANSPORT_KCP;
}

static bool flowie_transport_websocket(flowie_transport transport) {
  return transport == TF_NET_TRANSPORT_WS || transport == TF_NET_TRANSPORT_WSS;
}

static char *flowie_string_copy(const char *value) {
  char *copy;
  size_t size;
  if (value == NULL) return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy != NULL) memcpy(copy, value, size);
  return copy;
}

static bool flowie_token_valid(const char *value) {
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

static flowie_connection flowie_stream_handle(const flowie_server_impl *server,
                                               cnet_connection connection) {
  return (flowie_connection){(connection.slot - 1u) * server->owner_count +
                                server->owner_index + 1u,
                            connection.generation};
}

static flowie_server_impl *flowie_connection_owner(flowie_server_impl *root,
                                                   flowie_connection *connection) {
  flowie_server_impl *owner = root;
  if (root->owner_count == 1u) return root;
  if (connection->slot == 0u || connection->generation == 0u) return NULL;
  owner = root->owners[(connection->slot - 1u) % root->owner_count];
  connection->slot = (connection->slot - 1u) / root->owner_count + 1u;
  return connection->slot <= owner->config.stream.connection_capacity ? owner : NULL;
}

static void flowie_stream_recycle(void *user) {
  flowie_stream_peer *peer = (flowie_stream_peer *)user;
  flowie_server_impl *server = peer->owner;
  if (peer->ticket.generation != 0u) {
    const int status = cnet_handoff_release(&server->handoff, peer->ticket);
    if (server->management_status == SALTS_OK) server->management_status = status;
  }
  *peer = (flowie_stream_peer){0};
}

static flowie_connection flowie_packet_handle(cnet_packet_session session) {
  return (flowie_connection){session.slot, session.generation};
}

static bool flowie_handle_valid(flowie_connection connection) {
  return connection.slot != 0u && connection.generation != 0u;
}

static bool flowie_handle_equal(flowie_connection left, flowie_connection right) {
  return left.slot == right.slot && left.generation == right.generation;
}

static flowie_stream_peer *flowie_stream_peer_find(flowie_server_impl *server,
                                                    flowie_connection connection) {
  size_t index;
  for (index = 0u; index < server->config.stream.connection_capacity; ++index) {
    flowie_stream_peer *peer = &server->stream_peers[index];
    if (peer->used && peer->connection.slot == connection.slot &&
        peer->connection.generation == connection.generation)
      return peer;
  }
  return NULL;
}

static flowie_stream_peer *flowie_stream_peer_acquire(flowie_server_impl *server) {
  size_t index;
  for (index = 0u; index < server->config.stream.connection_capacity; ++index) {
    flowie_stream_peer *peer = &server->stream_peers[index];
    if (peer->used) continue;
    *peer = (flowie_stream_peer){.owner = server, .used = true};
    return peer;
  }
  return NULL;
}

static flowie_packet_peer *flowie_packet_peer_find(flowie_server_impl *server,
                                                    cnet_packet_session session) {
  flowie_packet_peer *peer;
  const size_t index = (size_t)session.slot - 1u;
  if (session.slot == 0u || index >= server->config.stream.connection_capacity) return NULL;
  peer = &server->packet_peers[index];
  return peer->opened && peer->generation == session.generation ? peer : NULL;
}

static void flowie_stream_state(void *user, cnet_connection connection,
                                cnet_connection_state state, const cnet_error *error) {
  flowie_stream_peer *peer = (flowie_stream_peer *)user;
  flowie_server_impl *server;
  int status = error == NULL ? SALTS_OK : error->status;
  if (peer == NULL || (server = peer->owner) == NULL || !peer->used) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    flowie_peer_info info = {.peer = peer->peer};
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
        server->config.observer.user, flowie_stream_handle(server, connection), &info);
    if (status == SALTS_OK) status = cnet_receive(&server->stream, connection, 1u);
    if (status != SALTS_OK) {
      peer->close_status = status;
      peer->close_status_set = true;
      (void)cnet_close(&server->stream, connection);
    }
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    const flowie_connection handle = flowie_stream_handle(server, connection);
    const bool opened = peer->opened;
    if (peer->close_status_set) status = peer->close_status;
    peer->opened = false;
    peer->close_status_set = false;
    peer->connection = (cnet_connection){0};
    if (opened) server->config.observer.on_close(server->config.observer.user, handle, status);
  }
}

static void flowie_stream_receive(void *user, cnet_connection connection,
                                  const cnet_receive_view *view) {
  flowie_stream_peer *peer = (flowie_stream_peer *)user;
  flowie_server_impl *server;
  int status;
  if (peer == NULL || view == NULL || (server = peer->owner) == NULL || !peer->used) return;
  status = server->config.observer.on_receive(server->config.observer.user,
                                               flowie_stream_handle(server, connection), view->data,
                                               view->size);
  if (status == SALTS_OK) status = cnet_receive(&server->stream, connection, 1u);
  if (status != SALTS_OK) {
    peer->close_status = status;
    peer->close_status_set = true;
    (void)cnet_close(&server->stream, connection);
  }
}

static void flowie_stream_send(void *user, cnet_connection connection, size_t size) {
  flowie_stream_peer *peer = (flowie_stream_peer *)user;
  if (peer == NULL || peer->owner == NULL || !peer->used) return;
  if (peer->owner->config.observer.on_send != NULL)
    peer->owner->config.observer.on_send(peer->owner->config.observer.user,
                                         flowie_stream_handle(peer->owner, connection), size);
}

static cnet_observer flowie_stream_observer(flowie_stream_peer *peer) {
  return (cnet_observer){.on_state = flowie_stream_state,
                         .on_receive = flowie_stream_receive,
                         .user = peer,
                         .on_send = flowie_stream_send};
}

static int flowie_packet_admit(void *user, cnet_packet_endpoint *endpoint,
                               cnet_packet_protocol protocol, const cnet_datagram_peer *peer,
                               uint32_t conversation) {
  flowie_server_impl *server = (flowie_server_impl *)user;
  (void)endpoint;
  (void)protocol;
  (void)peer;
  (void)conversation;
  return server == NULL || server->stop_requested ? SALTS_ESHUTDOWN : SALTS_OK;
}

static void flowie_packet_state(void *user, cnet_packet_endpoint *endpoint,
                                cnet_packet_session session, cnet_packet_session_state state,
                                const cnet_datagram_peer *peer, uint32_t conversation) {
  flowie_server_impl *server = (flowie_server_impl *)user;
  flowie_peer_info info = {0};
  int status;
  (void)endpoint;
  (void)conversation;
  if (server == NULL || peer == NULL) return;
  info.peer = (cnet_stream_peer){peer->family, peer->port, peer->scope_id, {0}};
  memcpy(info.peer.address, peer->address, sizeof(info.peer.address));
  if (state == CNET_PACKET_SESSION_OPEN) {
    flowie_packet_peer *record;
    const size_t index = (size_t)session.slot - 1u;
    if (session.slot == 0u || index >= server->config.stream.connection_capacity) {
      (void)cnet_packet_session_close(&server->packet, session);
      return;
    }
    record = &server->packet_peers[index];
    *record = (flowie_packet_peer){.generation = session.generation, .opened = true};
    status = server->config.observer.on_open(server->config.observer.user,
                                              flowie_packet_handle(session), &info);
    if (status != SALTS_OK) {
      record->close_status = status;
      record->close_status_set = true;
      (void)cnet_packet_session_close(&server->packet, session);
    }
  } else if (state == CNET_PACKET_SESSION_CLOSED) {
    flowie_packet_peer *record = flowie_packet_peer_find(server, session);
    if (record != NULL) {
      const int close_status = record->close_status_set ? record->close_status : SALTS_OK;
      *record = (flowie_packet_peer){0};
      server->config.observer.on_close(server->config.observer.user,
                                       flowie_packet_handle(session), close_status);
    }
  }
}

static void flowie_packet_receive(void *user, cnet_packet_endpoint *endpoint,
                                  cnet_packet_session session, const cnet_receive_view *view) {
  flowie_server_impl *server = (flowie_server_impl *)user;
  int status;
  (void)endpoint;
  if (server == NULL || view == NULL) return;
  status = server->config.observer.on_receive(server->config.observer.user,
                                               flowie_packet_handle(session), view->data,
                                               view->size);
  if (status != SALTS_OK) {
    flowie_packet_peer *peer = flowie_packet_peer_find(server, session);
    if (peer != NULL) {
      peer->close_status = status;
      peer->close_status_set = true;
    }
    (void)cnet_packet_session_close(&server->packet, session);
  }
}

static void flowie_packet_error(void *user, cnet_packet_endpoint *endpoint,
                                cnet_packet_session session, int status) {
  flowie_server_impl *server = (flowie_server_impl *)user;
  (void)endpoint;
  if (server != NULL) {
    flowie_packet_peer *peer = flowie_packet_peer_find(server, session);
    if (peer != NULL) {
      if (!peer->close_status_set) {
        peer->close_status = status;
        peer->close_status_set = true;
      }
      (void)cnet_packet_session_close(&server->packet, session);
    }
  }
}

static flowie_ws_peer *flowie_ws_peer_find_session(flowie_server_impl *server,
                                                    chttp_server_websocket_session session) {
  size_t index;
  for (index = 0u; index < server->config.stream.connection_capacity; ++index) {
    flowie_ws_peer *peer = &server->ws_peers[index];
    if (peer->used && peer->session.impl == session.impl &&
        peer->session.connection_slot == session.connection_slot &&
        peer->session.connection_generation == session.connection_generation &&
        peer->session.stream_id == session.stream_id)
      return peer;
  }
  return NULL;
}

static int flowie_ws_open(void *user, chttp_websocket *websocket,
                          const chttp_server_request_view *request,
                          chttp_server_response *response) {
  flowie_server_impl *server = (flowie_server_impl *)user;
  chttp_server_websocket_session session = {0};
  flowie_ws_peer *peer = NULL;
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
  cmeta_mutex_lock(&server->mutex);
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
  cmeta_mutex_unlock(&server->mutex);
  if (peer == NULL) return SALTS_ENOBUFS;
  status = server->config.observer.on_open(
      server->config.observer.user, (flowie_connection){(uint32_t)(index + 1u), peer->generation},
      &peer->peer);
  if (status != SALTS_OK) {
    cmeta_mutex_lock(&server->mutex);
    peer->used = false;
    peer->opened = false;
    cmeta_mutex_unlock(&server->mutex);
  }
  return status;
}

static void flowie_ws_event(void *user, chttp_websocket *websocket,
                            const chttp_websocket_event *event) {
  flowie_server_impl *server = (flowie_server_impl *)user;
  chttp_server_websocket_session session = {0};
  flowie_connection handle;
  bool opened = false;
  bool close_status_set = false;
  int close_status = SALTS_OK;
  int status = SALTS_OK;
  if (server == NULL || event == NULL ||
      chttp_server_websocket_session_capture(websocket, &session) != SALTS_OK)
    return;
  cmeta_mutex_lock(&server->mutex);
  {
    flowie_ws_peer *peer = flowie_ws_peer_find_session(server, session);
    if (peer != NULL) {
    handle = (flowie_connection){(uint32_t)(peer - server->ws_peers + 1u), peer->generation};
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
      handle = (flowie_connection){0};
    }
  }
  cmeta_mutex_unlock(&server->mutex);
  if (!flowie_handle_valid(handle)) return;
  if (event->kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
    status = event->message_type == CHTTP_WEBSOCKET_MESSAGE_BINARY
                 ? server->config.observer.on_receive(server->config.observer.user, handle,
                                                       event->data, event->size)
                 : SALTS_EPROTO;
    if (status != SALTS_OK) {
      const size_t index = (size_t)handle.slot - 1u;
      cmeta_mutex_lock(&server->mutex);
      if (index < server->config.stream.connection_capacity && server->ws_peers[index].used &&
          server->ws_peers[index].generation == handle.generation) {
        server->ws_peers[index].close_status = status;
        server->ws_peers[index].close_status_set = true;
      }
      cmeta_mutex_unlock(&server->mutex);
      (void)chttp_websocket_close(websocket, 1002u, NULL, 0u);
    }
  } else if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
    if (opened)
      server->config.observer.on_close(server->config.observer.user, handle,
                                       close_status_set ? close_status : SALTS_OK);
  }
}

static int flowie_command_progress(flowie_server_impl *server) {
  for (;;) {
    flowie_command command;
    const unsigned char *data;
    int status;
    cmeta_mutex_lock(&server->mutex);
    if (server->command_count == 0u) {
      cmeta_mutex_unlock(&server->mutex);
      return SALTS_OK;
    }
    command = server->commands[server->command_head];
    data = server->command_storage + command.data_offset;
    cmeta_mutex_unlock(&server->mutex);
    if (flowie_transport_stream(server->config.transport)) {
      flowie_stream_peer *peer = flowie_stream_peer_find(server, command.connection);
      if (peer == NULL)
        status = SALTS_ENOENT;
      else if (command.kind == TF_NET_COMMAND_SEND) {
        mem_buffer_t *buffer = mem_get_buffer(mem_global(), command.size);
        if (buffer == NULL) return SALTS_ENOMEM;
        /* The mailbox slot can be reused before CNet completes this send. */
        memcpy(mem_buffer_data(buffer), data, command.size);
        mem_set_used(buffer, command.size);
        status = cnet_send_buffer(&server->stream, peer->connection, buffer);
        mem_buffer_release(buffer);
      }
      else if (command.kind == TF_NET_COMMAND_SEND_SLICES)
        status = cnet_send_slicev(&server->stream, peer->connection, command.slices,
                                  command.slice_count);
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
      flowie_packet_peer *peer = flowie_packet_peer_find(server, session);
      if (peer != NULL) {
        peer->close_status = command.status;
        peer->close_status_set = true;
      }
      status = cnet_packet_session_close(&server->packet, session);
    }
    if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) return SALTS_OK;
    cmeta_mutex_lock(&server->mutex);
    server->commands[server->command_head] = (flowie_command){0};
    server->command_head = (server->command_head + 1u) % server->config.command_capacity;
    --server->command_count;
    if (command.reserved_bytes != 0u) {
      server->command_byte_head = (server->command_byte_head + command.reserved_bytes) %
                                  server->config.command_bytes_capacity;
      server->command_bytes_used -= command.reserved_bytes;
    }
    server->command_payload_bytes_used -= command.size;
    if (server->command_bytes_used == 0u) {
      server->command_byte_head = 0u;
      server->command_byte_tail = 0u;
    }
    cmeta_mutex_unlock(&server->mutex);
    flowie_command_slices_release(&command);
  }
}

/* The owner consumes a detached stream. Peer storage remains borrowed by the
 * manager until recycle, including immediate adoption failure. */
static int flowie_stream_adopt(flowie_server_impl *server, cnet_accepted_stream *accepted,
                                cnet_handoff_ticket ticket) {
  flowie_stream_peer *peer = flowie_stream_peer_acquire(server);
  int status = SALTS_ENOBUFS;
  if (peer != NULL) {
    const cnet_manager_attachment attachment = {
        .observer = flowie_stream_observer(peer), .on_recycle = flowie_stream_recycle};
    peer->peer = accepted->peer;
    peer->ticket = ticket;
    status = cnet_manager_reserve(&server->manager, &attachment, &peer->managed);
    if (status == SALTS_OK)
      return cnet_manager_adopt(&server->manager, peer->managed, accepted,
          server->tls_initialized ? &server->tls : NULL, &peer->connection);
    *peer = (flowie_stream_peer){0};
  }
  (void)cnet_accepted_stream_close(accepted);
  if (ticket.generation != 0u) {
    const int released = cnet_handoff_release(&server->handoff, ticket);
    if (released != SALTS_OK) return released;
  }
  return status;
}

static int flowie_stream_accept(flowie_server_impl *server) {
  for (;;) {
    cnet_manager_snapshot snapshot;
    cnet_accepted_stream accepted = {0};
    int status = cnet_manager_get_snapshot(&server->manager, &snapshot);
    if (status != SALTS_OK) return status;
    if (snapshot.reserved + snapshot.bound + snapshot.retired == snapshot.record_capacity)
      return SALTS_OK;
    status = cnet_listener_accept_detached(&server->listener, &accepted);
    if (status == SALTS_ETIMEDOUT) return SALTS_OK;
    if (status != SALTS_OK) return status;
    status = flowie_stream_adopt(server, &accepted, (cnet_handoff_ticket){0});
    if (status != SALTS_OK) return status;
  }
}

static bool flowie_should_stop(flowie_server_impl *server) {
  bool stop;
  cmeta_mutex_lock(&server->mutex);
  stop = server->stop_requested;
  cmeta_mutex_unlock(&server->mutex);
  return stop;
}

static void flowie_request_stop(flowie_server_impl *server) {
  cmeta_mutex_lock(&server->mutex);
  server->stop_requested = true;
  cmeta_mutex_unlock(&server->mutex);
  if (server->handoff.impl != NULL) (void)cnet_handoff_seal(&server->handoff);
  if (server->stream_initialized) (void)cnet_client_wake(&server->stream);
  if (server->packet_initialized) (void)cnet_packet_wake(&server->packet);
}

static void flowie_group_request_stop(flowie_server_impl *root) {
  uint32_t index;
  if (root->owners == NULL) {
    flowie_request_stop(root);
    return;
  }
  for (index = 0u; index < root->owner_count; ++index)
    if (root->owners[index] != NULL) flowie_request_stop(root->owners[index]);
}

/* The CNet chooser makes an advisory decision; only a successful Handoff
 * reservation admits this stream. Neither the Owner nor the MQTT session
 * migrates after adoption. */
static int flowie_owner_reserve(flowie_server_impl *root, uint32_t *next,
                                flowie_server_impl **out, cnet_handoff_ticket *ticket) {
  cnet_owner_placement_hint hints[FLOWIE_NETWORK_WORKERS_MAX] = {{0}};
  cnet_owner_placement_input placement = {0};
  size_t selected_index = SIZE_MAX;
  int status;
  if (root == NULL || next == NULL || out == NULL || ticket == NULL ||
      root->owner_count < 2u || root->owner_count > FLOWIE_NETWORK_WORKERS_MAX)
    return SALTS_EINVAL;
  *out = NULL;
  for (size_t index = 0u; index < root->owner_count; ++index) {
    flowie_server_impl *candidate = root->owners[index];
    cnet_handoff_snapshot snapshot = {0};
    uint64_t occupied;
    if (candidate == NULL) return SALTS_EINVAL;
    status = cnet_handoff_get_snapshot(&candidate->handoff, &snapshot);
    if (status != SALTS_OK) return status;
    occupied = (uint64_t)snapshot.reserved + snapshot.queued + snapshot.taken;
    hints[index].eligible = !snapshot.sealed && occupied < snapshot.connection_capacity;
    hints[index].pressure = occupied;
  }
  placement.size = sizeof(placement);
  placement.version = CNET_OWNER_PLACEMENT_VERSION;
  placement.kind = root->config.network_policy == TF_NET_OWNER_ROUND_ROBIN
                       ? CNET_OWNER_PLACE_ROUND_ROBIN
                       : CNET_OWNER_PLACE_LOWEST_PRESSURE;
  placement.owners = hints;
  placement.owner_count = root->owner_count;
  placement.sequence = *next;
  status = cnet_owner_placement_choose(&placement, &selected_index);
  if (status != SALTS_OK) return status;
  flowie_server_impl *selected = root->owners[selected_index];
  status = cnet_handoff_reserve(&selected->handoff, ticket);
  if (status != SALTS_OK) return status;
  *out = selected;
  *next = (uint32_t)((selected_index + 1u) % root->owner_count);
  return SALTS_OK;
}

/* Listener ownership never migrates after this thread starts. Admission reserves
 * capacity before accept; a published descriptor is moved once to its final owner. */
static void flowie_accept_worker(void *user) {
  flowie_server_impl *root = (flowie_server_impl *)user;
  uint32_t next = 0u;
  int status = SALTS_OK;
  while (!flowie_should_stop(root)) {
    flowie_server_impl *owner = NULL;
    cnet_accepted_stream accepted = {0};
    cnet_handoff_ticket ticket = {0};
    int ready = 0;
    status = cnet_listener_wait(&root->listener, root->config.poll_slice_ms, &ready);
    if (status != SALTS_OK) break;
    if (!ready) continue;
    status = flowie_owner_reserve(root, &next, &owner, &ticket);
    if (status == SALTS_ESHUTDOWN) { status = SALTS_OK; break; }
    if (status == SALTS_ENOBUFS) {
      status = SALTS_OK;
      /* Leave connections in the bounded listener backlog when every owner is full. */
      cmeta_sleep_ms(root->config.poll_slice_ms);
      continue;
    }
    if (status != SALTS_OK) break;
    status = cnet_listener_accept_detached(&root->listener, &accepted);
    if (status != SALTS_OK) {
      const int released = cnet_handoff_release(&owner->handoff, ticket);
      if (released != SALTS_OK) { status = released; break; }
      if (status == SALTS_ETIMEDOUT) { status = SALTS_OK; continue; }
      break;
    }
    status = cnet_handoff_publish(&owner->handoff, ticket, &accepted);
    if (status != SALTS_OK) {
      (void)cnet_accepted_stream_close(&accepted);
      const int released = cnet_handoff_release(&owner->handoff, ticket);
      if (released != SALTS_OK) { status = released; break; }
      if (status == SALTS_ESHUTDOWN) status = SALTS_OK;
      break;
    }
    /* Publication transfers ownership even if wake fails. Stop all owners and
     * let their bounded polling/drain path consume the published entry. */
    status = cnet_client_wake(&owner->stream);
    if (status != SALTS_OK) break;
  }
  (void)cnet_listener_close(&root->listener);
  if (status != SALTS_OK) {
    cmeta_mutex_lock(&root->mutex);
    if (root->terminal_status == SALTS_OK) root->terminal_status = status;
    cmeta_mutex_unlock(&root->mutex);
    flowie_group_request_stop(root);
  }
  cmeta_mutex_lock(&root->mutex);
  root->accept_done = true;
  cmeta_cond_broadcast(&root->changed);
  cmeta_mutex_unlock(&root->mutex);
}

static int flowie_accepted_progress(flowie_server_impl *server, bool closing) {
  for (;;) {
    cnet_accepted_stream accepted = {0};
    cnet_handoff_ticket ticket = {0};
    int status = cnet_handoff_take(&server->handoff, &ticket, &accepted);
    if (status == SALTS_ENOENT) return SALTS_OK;
    if (status != SALTS_OK) return status;
    if (closing) {
      const int closed = cnet_accepted_stream_close(&accepted);
      status = cnet_handoff_release(&server->handoff, ticket);
      if (closed != SALTS_OK) return closed;
    } else {
      status = flowie_stream_adopt(server, &accepted, ticket);
    }
    if (status != SALTS_OK) return status;
  }
}

static void flowie_worker_finish(flowie_server_impl *server, int status) {
  cmeta_mutex_lock(&server->mutex);
  if (server->terminal_status == SALTS_OK) server->terminal_status = status;
  server->worker_done = true;
  cmeta_cond_broadcast(&server->changed);
  cmeta_mutex_unlock(&server->mutex);
}

static void flowie_worker(void *user) {
  flowie_server_impl *server = (flowie_server_impl *)user;
  int status = server->config.network_cpu_count == 0u ? SALTS_OK :
      flowie_affinity_bind(server->config.network_cpus[server->owner_index]);
  if (status == SALTS_OK && flowie_transport_stream(server->config.transport)) {
    const cnet_manager_config config = {
        .size = sizeof(config), .version = CNET_MANAGER_VERSION, .client = &server->stream,
        .record_capacity = server->config.stream.connection_capacity,
        .connection_capacity = server->config.stream.connection_capacity};
    status = cnet_manager_init(&server->manager, &config);
  }
  cmeta_mutex_lock(&server->mutex);
  server->startup_status = status;
  server->worker_ready = true;
  cmeta_cond_broadcast(&server->changed);
  cmeta_mutex_unlock(&server->mutex);
  while (status == SALTS_OK && !flowie_should_stop(server)) {
    size_t events = 0u;
    status = flowie_command_progress(server);
    if (status != SALTS_OK) break;
    if (flowie_transport_stream(server->config.transport)) {
      int ready = 0;
      size_t work = 0u;
      status = cnet_manager_advance(&server->manager, server->config.stream.connection_capacity,
                                    &work);
      if (status == SALTS_OK) status = server->management_status;
      if (status != SALTS_OK) break;
      if (server->owner_count > 1u) {
        status = flowie_accepted_progress(server, false);
      } else {
        status = cnet_listener_wait(&server->listener, 0u, &ready);
        if (status == SALTS_OK && ready) status = flowie_stream_accept(server);
      }
      if (status == SALTS_OK)
        status = cnet_client_poll(&server->stream, server->config.poll_slice_ms, &events);
    } else {
      status = cnet_packet_poll(&server->packet, server->config.poll_slice_ms, &events);
    }
    if (status != SALTS_OK) break;
  }
  /* Also fence admission after an owner failure, before releasing handoffs. */
  flowie_group_request_stop(server->root);
  if (flowie_transport_stream(server->config.transport)) {
    int stop_status;
    if (server->owner_count > 1u) {
      const int drained = flowie_accepted_progress(server, true);
      if (status == SALTS_OK) status = drained;
    }
    else if (server->listener_initialized) (void)cnet_listener_close(&server->listener);
    if (server->manager.impl != NULL) {
      const int sealed = cnet_manager_seal(&server->manager);
      if (status == SALTS_OK) status = sealed;
    }
    do {
      stop_status = cnet_client_stop(&server->stream, server->config.poll_slice_ms);
    } while (stop_status == SALTS_ETIMEDOUT);
    if (status == SALTS_OK && stop_status != SALTS_OK && stop_status != SALTS_EALREADY)
      status = stop_status;
    if (server->manager.impl != NULL) {
      size_t work = 0u;
      int managed = cnet_manager_advance(&server->manager,
                                         server->config.stream.connection_capacity, &work);
      if (managed == SALTS_OK) managed = server->management_status;
      if (status == SALTS_OK) status = managed;
      managed = cnet_manager_destroy(&server->manager);
      if (status == SALTS_OK) status = managed;
    }
  } else {
    int stop_status;
    do {
      stop_status = cnet_packet_endpoint_stop(&server->packet, server->config.poll_slice_ms);
    } while (stop_status == SALTS_ETIMEDOUT);
    if (status == SALTS_OK && stop_status != SALTS_OK && stop_status != SALTS_EALREADY)
      status = stop_status;
  }
  flowie_worker_finish(server, status);
}

static void flowie_impl_free(flowie_server_impl *server) {
  size_t index;
  if (server == NULL) return;
  if (server->owners != NULL) {
    for (index = 1u; index < server->owner_count; ++index)
      flowie_impl_free(server->owners[index]);
    free(server->owners);
  }
  if (server->listener_initialized) (void)cnet_listener_close(&server->listener);
  if (server->packet_initialized) (void)cnet_packet_endpoint_stop(&server->packet, 0u);
  if (server->stream_initialized) (void)cnet_client_stop(&server->stream, 0u);
  if (server->websocket_initialized) (void)chttp_server_destroy(&server->websocket);
  if (server->packet_initialized) (void)cnet_packet_endpoint_destroy(&server->packet);
  if (server->stream_initialized) (void)cnet_client_destroy(&server->stream);
  if (server->listener_initialized) (void)cnet_listener_destroy(&server->listener);
  if (server->tls_initialized) (void)cnet_tls_server_destroy(&server->tls);
  if (server->sync_initialized) {
    cmeta_cond_destroy(&server->changed);
    cmeta_mutex_destroy(&server->mutex);
  }
  if (server->commands != NULL) {
    for (index = 0u; index < server->config.command_capacity; ++index)
      flowie_command_slices_release(&server->commands[index]);
  }
  free(server->command_storage);
  if (server->handoff.impl != NULL) (void)cnet_handoff_destroy(&server->handoff);
  free(server->commands);
  free(server->ws_peers);
  free(server->packet_peers);
  free(server->stream_peers);
  free(server->websocket_subprotocol);
  free(server->path);
  free(server->host);
  free(server);
}

static int flowie_init_stream(flowie_server_impl *server, bool with_listener) {
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
  if (server->owner_count > 1u) {
    const cnet_handoff_config handoff = {
        .size = sizeof(handoff), .version = CNET_HANDOFF_VERSION,
        .connection_capacity = server->config.stream.connection_capacity,
        .queue_capacity = server->config.stream.connection_capacity};
    status = cnet_handoff_init(&server->handoff, &handoff);
    if (status != SALTS_OK) return status;
  }
  if (!with_listener) return SALTS_OK;
  status = cnet_listener_init_ex(&server->listener, &listener,
                                 &server->config.listener_options);
  if (status != SALTS_OK) return status;
  server->listener_initialized = true;
  return cnet_listener_port(&server->listener, &server->port);
}

static int flowie_init_packet(flowie_server_impl *server) {
  cnet_packet_endpoint_config packet = server->config.packet;
  int status;
  packet.protocol = server->config.transport == TF_NET_TRANSPORT_UDP ? CNET_PACKET_UDP
                                                                     : CNET_PACKET_KCP;
  packet.session_capacity = server->config.stream.connection_capacity;
  packet.datagram.host = server->host;
  packet.datagram.port = server->config.port;
  packet.observer = (cnet_packet_observer){.on_admit = flowie_packet_admit,
                                           .on_state = flowie_packet_state,
                                           .on_receive = flowie_packet_receive,
                                           .on_error = flowie_packet_error,
                                           .user = server};
  packet.datagram.observer = (cnet_datagram_observer){0};
  packet.kcp.observer = (cnet_kcp_observer){0};
  server->config.packet = packet;
  status = cnet_packet_endpoint_init(&server->packet, &packet);
  if (status != SALTS_OK) return status;
  server->packet_initialized = true;
  return cnet_packet_endpoint_port(&server->packet, &server->port);
}

static int flowie_init_websocket(flowie_server_impl *server) {
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
                                          .on_open = flowie_ws_open,
                                          .on_event = flowie_ws_event,
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

static int flowie_owner_init(flowie_server *server, const flowie_server_config *config,
                              bool with_listener) {
  flowie_server_impl *impl;
  int status;
  if (server == NULL || config == NULL || server->impl != NULL ||
      config->size != sizeof(*config) || !flowie_transport_valid(config->transport) ||
      config->host == NULL || config->host[0] == '\0' || config->backlog == 0u ||
      config->stream.connection_capacity == 0u || !flowie_power_of_two(config->command_capacity) ||
      config->command_bytes_capacity == 0u || config->max_message_bytes == 0u ||
      config->command_bytes_capacity < config->max_message_bytes || config->poll_slice_ms == 0u ||
      config->observer.on_open == NULL || config->observer.on_receive == NULL ||
      config->observer.on_close == NULL ||
      (flowie_transport_websocket(config->transport) &&
       (config->path == NULL || config->path[0] != '/' ||
        (config->websocket_subprotocol != NULL &&
         !flowie_token_valid(config->websocket_subprotocol)))) ||
      ((config->transport == TF_NET_TRANSPORT_TLS || config->transport == TF_NET_TRANSPORT_WSS) &&
       config->tls == NULL))
    return SALTS_EINVAL;
  if (flowie_transport_websocket(config->transport) &&
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
  impl = (flowie_server_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->config = *config;
  impl->root = impl;
  impl->owner_count = config->network_workers ? config->network_workers : 1u;
  if (impl->config.stream_socket_options.size == 0u)
    impl->config.stream_socket_options =
        (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
  if (impl->config.listener_options.size == 0u)
    impl->config.listener_options = (cnet_listener_options)CNET_LISTENER_OPTIONS_INIT;
  impl->host = flowie_string_copy(config->host);
  impl->path = flowie_string_copy(config->path == NULL ? "/" : config->path);
  impl->websocket_subprotocol = flowie_string_copy(config->websocket_subprotocol);
  impl->commands = (flowie_command *)calloc(config->command_capacity, sizeof(*impl->commands));
  impl->command_storage = (unsigned char *)malloc(config->command_bytes_capacity);
  impl->stream_peers = (flowie_stream_peer *)calloc(config->stream.connection_capacity,
                                                     sizeof(*impl->stream_peers));
  impl->packet_peers = (flowie_packet_peer *)calloc(config->stream.connection_capacity,
                                                     sizeof(*impl->packet_peers));
  impl->ws_peers =
      (flowie_ws_peer *)calloc(config->stream.connection_capacity, sizeof(*impl->ws_peers));
  if (impl->host == NULL || impl->path == NULL ||
      (config->websocket_subprotocol != NULL && impl->websocket_subprotocol == NULL) ||
      impl->commands == NULL ||
      impl->command_storage == NULL || impl->stream_peers == NULL || impl->packet_peers == NULL ||
      impl->ws_peers == NULL) {
    flowie_impl_free(impl);
    return SALTS_ENOMEM;
  }
  cmeta_mutex_init(&impl->mutex);
  cmeta_cond_init(&impl->changed);
  impl->sync_initialized = true;
  impl->config.host = impl->host;
  impl->config.path = impl->path;
  impl->config.websocket_subprotocol = impl->websocket_subprotocol;
  if (flowie_transport_stream(config->transport))
    status = flowie_init_stream(impl, with_listener);
  else if (flowie_transport_packet(config->transport))
    status = flowie_init_packet(impl);
  else
    status = flowie_init_websocket(impl);
  if (status != SALTS_OK) {
    flowie_impl_free(impl);
    return status;
  }
  server->impl = impl;
  return SALTS_OK;
}

int flowie_server_init(flowie_server *server, const flowie_server_config *config) {
  flowie_server_impl *root = NULL;
  uint32_t count, index;
  if (server == NULL || server->impl != NULL || config == NULL ||
      config->size != sizeof(*config)) return SALTS_EINVAL;
  count = config->network_workers ? config->network_workers : 1u;
  if (config->network_policy != TF_NET_OWNER_ROUND_ROBIN &&
      config->network_policy != TF_NET_OWNER_LEAST_CONNECTIONS) return SALTS_EINVAL;
  if (config->network_cpu_count > FLOWIE_NETWORK_WORKERS_MAX ||
      (config->network_cpu_count != 0u && config->network_cpu_count != count))
    return SALTS_EINVAL;
  if (count > FLOWIE_NETWORK_WORKERS_MAX ||
      (count > 1u && count > config->stream.connection_capacity) ||
      config->stream.connection_capacity > UINT32_MAX) return SALTS_ERANGE;
  if ((count > 1u || config->network_policy != TF_NET_OWNER_ROUND_ROBIN ||
       config->network_cpu_count != 0u) && !flowie_transport_stream(config->transport))
    return SALTS_ENOTSUP;
  if (count > 1u && config->command_bytes_capacity / count < config->max_message_bytes)
    return SALTS_ERANGE;
  for (index = 0u; index < count; ++index) {
    flowie_server owner = {0};
    flowie_server_config partition = *config;
    int status;
    partition.stream.connection_capacity = config->stream.connection_capacity / count +
                                           (index < config->stream.connection_capacity % count);
    partition.command_bytes_capacity = config->command_bytes_capacity / count +
                                       (index < config->command_bytes_capacity % count);
    status = flowie_owner_init(&owner, &partition, index == 0u);
    if (status != SALTS_OK) { flowie_impl_free(root); return status; }
    if (index == 0u) {
      root = (flowie_server_impl *)owner.impl;
      if (count > 1u) {
        root->owners = (flowie_server_impl **)calloc(count, sizeof(*root->owners));
        if (root->owners == NULL) { flowie_impl_free(root); return SALTS_ENOMEM; }
        root->owners[0] = root;
      }
    } else {
      flowie_server_impl *child = (flowie_server_impl *)owner.impl;
      root->owners[index] = child;
      child->root = root;
      child->owner_index = index;
    }
  }
  server->impl = root;
  return SALTS_OK;
}

static int flowie_owner_wait_ready(flowie_server_impl *owner) {
  int status;
  cmeta_mutex_lock(&owner->mutex);
  while (!owner->worker_ready) cmeta_cond_wait(&owner->changed, &owner->mutex);
  status = owner->startup_status;
  cmeta_mutex_unlock(&owner->mutex);
  return status;
}

int flowie_server_start(flowie_server *server) {
  flowie_server_impl *impl;
  int status;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (flowie_server_impl *)server->impl;
  if (impl->started) return SALTS_EALREADY;
  if (impl->stop_requested || impl->worker_done) return SALTS_ESHUTDOWN;
  if (impl->owner_count > 1u) {
    uint32_t index;
    for (index = 0u; index < impl->owner_count; ++index) {
      flowie_server_impl *owner = impl->owners[index];
      owner->started = true;
      status = cmeta_thread_create(&owner->thread, flowie_worker, owner);
      if (status != SALTS_OK) {
        cmeta_mutex_lock(&owner->mutex);
        owner->worker_done = true;
        cmeta_mutex_unlock(&owner->mutex);
        (void)flowie_server_stop(server, 0u);
        return SALTS_EIO;
      }
      owner->thread_started = true;
      status = flowie_owner_wait_ready(owner);
      if (status != SALTS_OK) {
        (void)flowie_server_stop(server, 0u);
        return status;
      }
    }
    status = cmeta_thread_create(&impl->accept_thread, flowie_accept_worker, impl);
    if (status != SALTS_OK) {
      (void)flowie_server_stop(server, 0u);
      return SALTS_EIO;
    }
    impl->accept_thread_started = true;
    return SALTS_OK;
  }
  if (flowie_transport_websocket(impl->config.transport)) {
    status = chttp_server_start(&impl->websocket);
    if (status == SALTS_OK) status = chttp_server_port(&impl->websocket, &impl->port);
  } else {
    impl->started = true;
    status = cmeta_thread_create(&impl->thread, flowie_worker, impl);
    if (status == SALTS_OK) {
      impl->thread_started = true;
      status = flowie_owner_wait_ready(impl);
      if (status != SALTS_OK) (void)flowie_server_stop(server, 0u);
    }
    else { impl->started = false; status = SALTS_EIO; }
  }
  if (status == SALTS_OK && flowie_transport_websocket(impl->config.transport))
    impl->started = true;
  return status;
}

int flowie_server_port(const flowie_server *server, uint16_t *out_port) {
  const flowie_server_impl *impl;
  if (server == NULL || server->impl == NULL || out_port == NULL) return SALTS_EINVAL;
  impl = (const flowie_server_impl *)server->impl;
  *out_port = impl->port;
  return SALTS_OK;
}

static int flowie_command_submit(flowie_server_impl *server, flowie_connection connection,
                                 flowie_command_kind kind, const void *data, size_t size,
                                 int close_status) {
  flowie_command *command;
  unsigned char *storage;
  size_t data_offset;
  size_t reserved_bytes;
  size_t tail;
  if (!flowie_handle_valid(connection) || (data == NULL && size != 0u) ||
      size > server->config.max_message_bytes)
    return SALTS_EINVAL;
  cmeta_mutex_lock(&server->mutex);
  if (!server->started || server->stop_requested || server->worker_done) {
    cmeta_mutex_unlock(&server->mutex);
    return SALTS_ESHUTDOWN;
  }
  if (server->command_count == server->config.command_capacity) {
    cmeta_mutex_unlock(&server->mutex);
    return SALTS_ENOBUFS;
  }
  if (size > server->config.command_bytes_capacity - server->command_payload_bytes_used) {
    cmeta_mutex_unlock(&server->mutex);
    return SALTS_ENOBUFS;
  }
  if (size > server->config.command_bytes_capacity - server->command_bytes_used) {
    cmeta_mutex_unlock(&server->mutex);
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
      cmeta_mutex_unlock(&server->mutex);
      return SALTS_ENOBUFS;
    }
    data_offset = 0u;
    reserved_bytes += padding;
  } else if (size != 0u && server->command_byte_tail < server->command_byte_head &&
             size > server->command_byte_head - server->command_byte_tail) {
    cmeta_mutex_unlock(&server->mutex);
    return SALTS_ENOBUFS;
  }
  tail = (server->command_head + server->command_count) % server->config.command_capacity;
  command = &server->commands[tail];
  storage = server->command_storage + data_offset;
  if (size != 0u) memcpy(storage, data, size);
  *command = (flowie_command){.connection = connection,
                              .data_offset = data_offset,
                              .reserved_bytes = reserved_bytes,
                              .size = size,
                              .status = close_status,
                              .kind = kind};
  ++server->command_count;
  server->command_byte_tail =
      (server->command_byte_tail + reserved_bytes) % server->config.command_bytes_capacity;
  server->command_bytes_used += reserved_bytes;
  server->command_payload_bytes_used += size;
  cmeta_mutex_unlock(&server->mutex);
  if (flowie_transport_stream(server->config.transport))
    (void)cnet_client_wake(&server->stream);
  else
    (void)cnet_packet_wake(&server->packet);
  return SALTS_OK;
}

static int flowie_command_submit_slicev(flowie_server_impl *server,
                                        flowie_connection connection,
                                        const mem_slice_t *segments, size_t segment_count) {
  mem_slice_t *owned = NULL;
  flowie_command *command;
  size_t total_size = 0u;
  size_t tail;
  size_t index;
  int status = SALTS_OK;
  if (server == NULL || !flowie_handle_valid(connection) || segments == NULL ||
      segment_count == 0u || segment_count > CNET_RETAINED_VECTOR_MAX ||
      !flowie_transport_stream(server->config.transport))
    return SALTS_EINVAL;
  owned = (mem_slice_t *)calloc(segment_count, sizeof(*owned));
  if (owned == NULL) return SALTS_ENOMEM;
  for (index = 0u; index < segment_count; ++index) {
    status = flowie_command_slice_clone(&segments[index], &owned[index]);
    if (status != SALTS_OK) goto fail;
    if (owned[index].length > SIZE_MAX - total_size) {
      status = SALTS_ERANGE;
      goto fail;
    }
    total_size += owned[index].length;
  }
  if (total_size == 0u || total_size > server->config.stream.max_send_bytes) {
    status = SALTS_EMSGSIZE;
    goto fail;
  }
  status = flowie_send_batch_coalesce(owned, &segment_count);
  if (status != SALTS_OK) goto fail;
  cmeta_mutex_lock(&server->mutex);
  if (!server->started || server->stop_requested || server->worker_done) {
    status = SALTS_ESHUTDOWN;
  } else if (server->command_count == server->config.command_capacity ||
             total_size > server->config.command_bytes_capacity -
                              server->command_payload_bytes_used) {
    status = SALTS_ENOBUFS;
  } else {
    tail = (server->command_head + server->command_count) % server->config.command_capacity;
    command = &server->commands[tail];
    *command = (flowie_command){.connection = connection,
                                .size = total_size,
                                .slices = owned,
                                .slice_count = segment_count,
                                .kind = TF_NET_COMMAND_SEND_SLICES};
    ++server->command_count;
    server->command_payload_bytes_used += total_size;
    owned = NULL;
  }
  cmeta_mutex_unlock(&server->mutex);
  if (status == SALTS_OK) (void)cnet_client_wake(&server->stream);
fail:
  if (owned != NULL) {
    flowie_command cleanup = {.slices = owned, .slice_count = segment_count};
    flowie_command_slices_release(&cleanup);
  }
  return status;
}

static bool flowie_ws_session_copy(flowie_server_impl *server, flowie_connection connection,
                                   chttp_server_websocket_session *out_session) {
  flowie_ws_peer *peer;
  const size_t index = (size_t)connection.slot - 1u;
  bool found;
  if (connection.slot == 0u || index >= server->config.stream.connection_capacity) return false;
  cmeta_mutex_lock(&server->mutex);
  peer = &server->ws_peers[index];
  if (!peer->used || peer->generation != connection.generation) peer = NULL;
  if (peer != NULL) *out_session = peer->session;
  found = peer != NULL;
  cmeta_mutex_unlock(&server->mutex);
  return found;
}

int flowie_server_send(flowie_server *server, flowie_connection connection, const void *data,
                       size_t size) {
  flowie_server_impl *impl;
  if (server == NULL || server->impl == NULL || data == NULL || size == 0u)
    return SALTS_EINVAL;
  impl = (flowie_server_impl *)server->impl;
  if (size > impl->config.max_message_bytes) return SALTS_EMSGSIZE;
  if (flowie_transport_websocket(impl->config.transport)) {
    chttp_server_websocket_session session;
    if (!flowie_ws_session_copy(impl, connection, &session)) return SALTS_ENOENT;
    return chttp_server_websocket_send_binary(&session, data, size);
  }
  impl = flowie_connection_owner(impl, &connection);
  if (impl == NULL) return SALTS_ENOENT;
  return flowie_command_submit(impl, connection, TF_NET_COMMAND_SEND, data, size, SALTS_OK);
}

int flowie_server_send_slicev(flowie_server *server, flowie_connection connection,
                              const mem_slice_t *segments, size_t segment_count) {
  flowie_server_impl *impl;
  if (server == NULL || server->impl == NULL || segments == NULL || segment_count == 0u)
    return SALTS_EINVAL;
  impl = (flowie_server_impl *)server->impl;
  if (!flowie_transport_stream(impl->config.transport)) return SALTS_ENOTSUP;
  impl = flowie_connection_owner(impl, &connection);
  if (impl == NULL) return SALTS_ENOENT;
  return flowie_command_submit_slicev(impl, connection, segments, segment_count);
}

int flowie_server_close(flowie_server *server, flowie_connection connection, int status) {
  flowie_server_impl *impl;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (flowie_server_impl *)server->impl;
  if (flowie_transport_websocket(impl->config.transport)) {
    chttp_server_websocket_session session;
    const size_t index = (size_t)connection.slot - 1u;
    int close_result;
    if (!flowie_ws_session_copy(impl, connection, &session)) return SALTS_ENOENT;
    cmeta_mutex_lock(&impl->mutex);
    if (index < impl->config.stream.connection_capacity && impl->ws_peers[index].used &&
        impl->ws_peers[index].generation == connection.generation) {
      impl->ws_peers[index].close_status = status;
      impl->ws_peers[index].close_status_set = true;
    }
    cmeta_mutex_unlock(&impl->mutex);
    close_result = chttp_server_websocket_close(&session, 1000u, NULL, 0u);
    if (close_result != SALTS_OK) {
      cmeta_mutex_lock(&impl->mutex);
      if (index < impl->config.stream.connection_capacity && impl->ws_peers[index].used &&
          impl->ws_peers[index].generation == connection.generation &&
          impl->ws_peers[index].close_status_set &&
          impl->ws_peers[index].close_status == status)
        impl->ws_peers[index].close_status_set = false;
      cmeta_mutex_unlock(&impl->mutex);
    }
    return close_result;
  }
  impl = flowie_connection_owner(impl, &connection);
  if (impl == NULL) return SALTS_ENOENT;
  return flowie_command_submit(impl, connection, TF_NET_COMMAND_CLOSE, NULL, 0u, status);
}

static int flowie_owner_stop(flowie_server *server, uint32_t timeout_ms) {
  flowie_server_impl *impl;
  uint64_t started_ms;
  int status;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (flowie_server_impl *)server->impl;
  if (!impl->started) return SALTS_OK;
  if (flowie_transport_websocket(impl->config.transport)) {
    status = chttp_server_stop(&impl->websocket, timeout_ms);
    if (status != SALTS_ETIMEDOUT && status != SALTS_EBUSY) impl->started = false;
    return status;
  }
  started_ms = cmeta_monotonic_ms();
  cmeta_mutex_lock(&impl->mutex);
  impl->stop_requested = true;
  cmeta_mutex_unlock(&impl->mutex);
  if (flowie_transport_stream(impl->config.transport))
    (void)cnet_client_wake(&impl->stream);
  else
    (void)cnet_packet_wake(&impl->packet);
  cmeta_mutex_lock(&impl->mutex);
  while (!impl->worker_done) {
    const uint64_t elapsed = cmeta_monotonic_ms() - started_ms;
    if (timeout_ms != 0u && elapsed >= timeout_ms) {
      cmeta_mutex_unlock(&impl->mutex);
      return SALTS_ETIMEDOUT;
    }
    if (timeout_ms == 0u)
      cmeta_cond_wait(&impl->changed, &impl->mutex);
    else if (cmeta_cond_timedwait(&impl->changed, &impl->mutex,
                                  ((uint64_t)timeout_ms - elapsed) * 1000000u) != SALTS_OK &&
             !impl->worker_done) {
      cmeta_mutex_unlock(&impl->mutex);
      return SALTS_ETIMEDOUT;
    }
  }
  status = impl->terminal_status;
  cmeta_mutex_unlock(&impl->mutex);
  if (impl->thread_started) {
    if (cmeta_thread_join(&impl->thread) != SALTS_OK) return SALTS_EIO;
    cmeta_thread_destroy(&impl->thread);
    impl->thread_started = false;
  }
  cmeta_mutex_lock(&impl->mutex);
  impl->started = false;
  cmeta_mutex_unlock(&impl->mutex);
  return status;
}

int flowie_server_stop(flowie_server *server, uint32_t timeout_ms) {
  flowie_server_impl *root;
  uint64_t started_ms;
  uint32_t index;
  int result = SALTS_OK;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  root = (flowie_server_impl *)server->impl;
  if (root->owner_count <= 1u) return flowie_owner_stop(server, timeout_ms);
  if (!root->started && !root->stop_requested) return SALTS_OK;
  started_ms = cmeta_monotonic_ms();
  flowie_group_request_stop(root);
  for (index = 0u; index < root->owner_count; ++index) {
    flowie_server owner = {root->owners[index]};
    uint32_t remaining = 0u;
    int status;
    if (timeout_ms != 0u) {
      const uint64_t elapsed = cmeta_monotonic_ms() - started_ms;
      if (elapsed >= timeout_ms) return SALTS_ETIMEDOUT;
      remaining = timeout_ms - (uint32_t)elapsed;
    }
    status = flowie_owner_stop(&owner, remaining);
    if (status == SALTS_ETIMEDOUT || status == SALTS_EBUSY) return status;
    if (result == SALTS_OK) result = status;
  }
  if (root->accept_thread_started) {
    cmeta_mutex_lock(&root->mutex);
    while (!root->accept_done) {
      const uint64_t elapsed = cmeta_monotonic_ms() - started_ms;
      if (timeout_ms != 0u && elapsed >= timeout_ms) {
        cmeta_mutex_unlock(&root->mutex);
        return SALTS_ETIMEDOUT;
      }
      if (timeout_ms == 0u) cmeta_cond_wait(&root->changed, &root->mutex);
      else if (cmeta_cond_timedwait(&root->changed, &root->mutex,
                                     ((uint64_t)timeout_ms - elapsed) * 1000000u) != SALTS_OK &&
               !root->accept_done) {
        cmeta_mutex_unlock(&root->mutex);
        return SALTS_ETIMEDOUT;
      }
    }
    cmeta_mutex_unlock(&root->mutex);
    if (cmeta_thread_join(&root->accept_thread) != SALTS_OK) return SALTS_EIO;
    cmeta_thread_destroy(&root->accept_thread);
    root->accept_thread_started = false;
  }
  if (result == SALTS_OK) result = root->terminal_status;
  return result;
}

int flowie_server_destroy(flowie_server *server) {
  flowie_server_impl *impl;
  if (server == NULL) return SALTS_EINVAL;
  if (server->impl == NULL) return SALTS_OK;
  impl = (flowie_server_impl *)server->impl;
  if (impl->started || impl->thread_started || impl->accept_thread_started) return SALTS_EBUSY;
  if (impl->owners != NULL) {
    uint32_t index;
    for (index = 1u; index < impl->owner_count; ++index)
      if (impl->owners[index]->started || impl->owners[index]->thread_started) return SALTS_EBUSY;
  }
  for (uint32_t index = 0u; index < impl->owner_count; ++index) {
    flowie_server_impl *owner = impl->owners == NULL ? impl : impl->owners[index];
    if (owner->manager.impl != NULL) return SALTS_EBUSY;
    if (owner->handoff.impl != NULL) {
      cnet_handoff_snapshot snapshot;
      const int status = cnet_handoff_get_snapshot(&owner->handoff, &snapshot);
      if (status != SALTS_OK) return status;
      if (!snapshot.drained) return SALTS_EBUSY;
    }
  }
  flowie_impl_free(impl);
  server->impl = NULL;
  return SALTS_OK;
}
