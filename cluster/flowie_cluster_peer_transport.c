#include "flowie_stl_error_internal.h"

#include <cstl.h>

#include "flowie_cluster_peer_internal.h"

#include "salts_buffer.h"
#include "salts_thread.h"

#include <stdlib.h>
#include <string.h>

enum { FLOWIE_CLUSTER_PEER_POLL_SLICE_MS = 1000u };

typedef struct flowie_cluster_peer_send_s {
  tstr bytes;
  flowie_cluster_peer_send_complete_fn complete;
  void *complete_user_data;
} flowie_cluster_peer_send_t;

struct flowie_cluster_peer_link_s {
  flowie_cluster_peer_role_t role;
  size_t max_payload_size;
  size_t max_frame_size;
  size_t queue_entries_limit;
  size_t queue_bytes_limit;
  size_t pending_entries;
  size_t pending_bytes;
  salts_mutex_t mutex;
  deque_t queue;
  flowie_cluster_peer_link_state_t state;
  cnet_client *network;
  cnet_connection connection;
  int transport_connected;
  int transport_terminal;
  int transport_status;
  int receive_pending;
  int send_pending;
  size_t expected_send_size;
  tstr cluster_id;
  tstr local_node_id;
  uint8_t local_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE];
  tstr remote_node_id;
  uint8_t remote_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE];
  flowie_cluster_peer_authorize_fn authorize;
  flowie_cluster_peer_active_fn active;
  flowie_cluster_peer_receive_fn receive;
  void *user_data;
  mem_buffer_t *receive_buffer;
};

static int flowie_cluster_peer_link_config_text(vstr value, size_t maximum, int required) {
  if ((value.len != 0u && !value.data) || (required && value.len == 0u)) return SALTS_EINVAL;
  if (value.len > maximum) return SALTS_EMSGSIZE;
  return value.len != 0u && memchr(value.data, '\0', value.len) != NULL ? SALTS_EPROTO : SALTS_OK;
}

static int flowie_cluster_peer_link_nonzero(const uint8_t *value, size_t size) {
  size_t index;
  for (index = 0u; index < size; ++index)
    if (value[index] != 0u) return 1;
  return 0;
}

static int flowie_cluster_peer_link_config_validate(const flowie_cluster_peer_link_config_t *config,
                                                    size_t *max_frame_size) {
  size_t identity_overhead =
      FLOWIE_CLUSTER_ID_MAX + FLOWIE_CLUSTER_LISTENER_ID_MAX + FLOWIE_CLUSTER_NODE_ID_MAX * 2u;
  int rc;
  if (!config || config->size != sizeof(*config) ||
      config->abi_version != FLOWIE_CLUSTER_PEER_TRANSPORT_ABI_V1 || !max_frame_size ||
      (config->role != FLOWIE_CLUSTER_PEER_ROLE_INITIATOR &&
       config->role != FLOWIE_CLUSTER_PEER_ROLE_RESPONDER) ||
      config->max_payload_size == 0u || config->max_payload_size > UINT32_MAX ||
      config->queue_entries == 0u || config->queue_bytes == 0u || !config->authorize ||
      !config->receive) {
    return SALTS_EINVAL;
  }
  if (config->max_payload_size > SIZE_MAX - identity_overhead - FLOWIE_CLUSTER_PEER_HEADER_SIZE)
    return SALTS_ERANGE;
  *max_frame_size = FLOWIE_CLUSTER_PEER_HEADER_SIZE + identity_overhead + config->max_payload_size;
  if (*max_frame_size > UINT32_MAX) return SALTS_ERANGE;
  if (config->queue_entries > SIZE_MAX / sizeof(flowie_cluster_peer_send_t) ||
      config->queue_bytes < *max_frame_size) {
    return SALTS_EINVAL;
  }
  rc = flowie_cluster_peer_link_config_text(config->cluster_id, FLOWIE_CLUSTER_ID_MAX, 1);
  if (rc == SALTS_OK)
    rc = flowie_cluster_peer_link_config_text(config->local_node_id, FLOWIE_CLUSTER_NODE_ID_MAX, 1);
  if (rc == SALTS_OK)
    rc = flowie_cluster_peer_link_config_text(config->remote_node_id, FLOWIE_CLUSTER_NODE_ID_MAX,
                                              config->role == FLOWIE_CLUSTER_PEER_ROLE_INITIATOR);
  if (rc != SALTS_OK) return rc;
  if (!flowie_cluster_peer_link_nonzero(config->local_boot_id, sizeof(config->local_boot_id)) ||
      (config->role == FLOWIE_CLUSTER_PEER_ROLE_INITIATOR &&
       !flowie_cluster_peer_link_nonzero(config->remote_boot_id, sizeof(config->remote_boot_id)))) {
    return SALTS_EINVAL;
  }
  return SALTS_OK;
}

static void flowie_cluster_peer_send_complete(flowie_cluster_peer_send_t *send, int status) {
  flowie_cluster_peer_send_complete_fn complete = send->complete;
  void *user_data = send->complete_user_data;
  tstr_free(send->bytes);
  memset(send, 0, sizeof(*send));
  if (complete) complete(user_data, status);
}

static int flowie_cluster_peer_link_pop(flowie_cluster_peer_link_t *link,
                                        flowie_cluster_peer_send_t *out) {
  int rc;
  salts_mutex_lock(&link->mutex);
  rc = flowie_stl_error(deque_pop_front(&link->queue, out));
  salts_mutex_unlock(&link->mutex);
  return rc;
}

static void flowie_cluster_peer_link_complete_account(flowie_cluster_peer_link_t *link,
                                                      size_t bytes) {
  salts_mutex_lock(&link->mutex);
  if (link->pending_entries != 0u) --link->pending_entries;
  if (bytes <= link->pending_bytes) link->pending_bytes -= bytes;
  salts_mutex_unlock(&link->mutex);
}

static void flowie_cluster_peer_link_fail_pending(flowie_cluster_peer_link_t *link, int status) {
  flowie_cluster_peer_send_t send;
  while (flowie_cluster_peer_link_pop(link, &send) == SALTS_OK) {
    size_t bytes = tstr_len(send.bytes);
    flowie_cluster_peer_link_complete_account(link, bytes);
    flowie_cluster_peer_send_complete(&send, status);
  }
}

static int flowie_cluster_peer_connection_matches(const flowie_cluster_peer_link_t *link,
                                                   cnet_connection connection) {
  return link != NULL && connection.slot == link->connection.slot &&
         connection.generation == link->connection.generation;
}

static void flowie_cluster_peer_network_state(void *user, cnet_connection connection,
                                              cnet_connection_state state,
                                              const cnet_error *error) {
  flowie_cluster_peer_link_t *link = (flowie_cluster_peer_link_t *)user;
  if (link != NULL && link->connection.slot == 0u &&
      (state == CNET_CONNECTION_CONNECTING || state == CNET_CONNECTION_CONNECTED))
    link->connection = connection;
  if (!flowie_cluster_peer_connection_matches(link, connection)) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    link->transport_connected = 1;
    link->transport_status = SALTS_OK;
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    link->transport_connected = 0;
    link->transport_terminal = 1;
    link->receive_pending = 0;
    link->send_pending = 0;
    link->transport_status = error != NULL && error->status != SALTS_OK
                                 ? error->status
                                 : SALTS_ECONNRESET;
  }
}

static void flowie_cluster_peer_network_receive(void *user, cnet_connection connection,
                                                const cnet_receive_view *view) {
  flowie_cluster_peer_link_t *link = (flowie_cluster_peer_link_t *)user;
  size_t used;
  if (!flowie_cluster_peer_connection_matches(link, connection)) return;
  link->receive_pending = 0;
  if (view == NULL || view->kind != CNET_MESSAGE_BYTES || view->data == NULL || view->size == 0u) {
    link->transport_status = SALTS_EPROTO;
    return;
  }
  used = mem_buffer_used(link->receive_buffer);
  if (used > link->max_frame_size || view->size > link->max_frame_size - used) {
    link->transport_status = SALTS_EMSGSIZE;
    return;
  }
  memcpy(mem_buffer_data(link->receive_buffer) + used, view->data, view->size);
  mem_set_used(link->receive_buffer, used + view->size);
}

static void flowie_cluster_peer_network_send(void *user, cnet_connection connection, size_t size) {
  flowie_cluster_peer_link_t *link = (flowie_cluster_peer_link_t *)user;
  if (!flowie_cluster_peer_connection_matches(link, connection)) return;
  if (!link->send_pending || size != link->expected_send_size) {
    link->transport_status = SALTS_EPROTO;
    return;
  }
  link->send_pending = 0;
  link->expected_send_size = 0u;
}

int flowie_cluster_peer_link_create(const flowie_cluster_peer_link_config_t *config,
                                    flowie_cluster_peer_link_t **out) {
  flowie_cluster_peer_link_t *link;
  size_t max_frame_size = 0u;
  int rc;
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  rc = flowie_cluster_peer_link_config_validate(config, &max_frame_size);
  if (rc != SALTS_OK) return rc;
  link = (flowie_cluster_peer_link_t *)calloc(1u, sizeof(*link));
  if (!link) return SALTS_ENOMEM;
  link->role = config->role;
  link->max_payload_size = config->max_payload_size;
  link->max_frame_size = max_frame_size;
  link->queue_entries_limit = config->queue_entries;
  link->queue_bytes_limit = config->queue_bytes;
  link->state = FLOWIE_CLUSTER_PEER_LINK_CREATED;
  link->authorize = config->authorize;
  link->active = config->active;
  link->receive = config->receive;
  link->user_data = config->user_data;
  memcpy(link->local_boot_id, config->local_boot_id, sizeof(link->local_boot_id));
  memcpy(link->remote_boot_id, config->remote_boot_id, sizeof(link->remote_boot_id));
  link->cluster_id = tstr_from_v(config->cluster_id);
  link->local_node_id = tstr_from_v(config->local_node_id);
  if (config->remote_node_id.len != 0u) link->remote_node_id = tstr_from_v(config->remote_node_id);
  if (!link->cluster_id || !link->local_node_id ||
      (config->remote_node_id.len != 0u && !link->remote_node_id)) {
    rc = SALTS_ENOMEM;
    goto fail;
  }
  salts_mutex_init(&link->mutex);
  rc = flowie_stl_error(deque_init_bytes(&link->queue, sizeof(flowie_cluster_peer_send_t), _Alignof(flowie_cluster_peer_send_t), SIZE_MAX));
  if (rc != SALTS_OK) goto fail_mutex;
  rc = flowie_stl_error(deque_reserve(&link->queue, link->queue_entries_limit));
  if (rc != SALTS_OK) goto fail_queue;
  link->receive_buffer = mem_get_buffer(mem_global(), link->max_frame_size);
  if (!link->receive_buffer) {
    rc = SALTS_ENOMEM;
    goto fail_queue;
  }
  *out = link;
  return SALTS_OK;

fail_queue:
  deque_destroy(&link->queue);
fail_mutex:
  salts_mutex_destroy(&link->mutex);
fail:
  tstr_free(link->cluster_id);
  tstr_free(link->local_node_id);
  tstr_free(link->remote_node_id);
  free(link);
  return rc;
}

int flowie_cluster_peer_link_observer(flowie_cluster_peer_link_t *link, cnet_client *network,
                                      cnet_observer *out_observer) {
  if (link == NULL || network == NULL || network->impl == NULL || out_observer == NULL)
    return SALTS_EINVAL;
  salts_mutex_lock(&link->mutex);
  if (link->state != FLOWIE_CLUSTER_PEER_LINK_CREATED || link->network != NULL) {
    salts_mutex_unlock(&link->mutex);
    return SALTS_EBUSY;
  }
  link->network = network;
  link->transport_status = SALTS_OK;
  salts_mutex_unlock(&link->mutex);
  *out_observer = (cnet_observer){.on_state = flowie_cluster_peer_network_state,
                                  .on_receive = flowie_cluster_peer_network_receive,
                                  .on_send = flowie_cluster_peer_network_send,
                                  .user = link};
  return SALTS_OK;
}

int flowie_cluster_peer_link_destroy(flowie_cluster_peer_link_t *link) {
  if (!link) return SALTS_OK;
  salts_mutex_lock(&link->mutex);
  if (link->state == FLOWIE_CLUSTER_PEER_LINK_HANDSHAKING ||
      link->state == FLOWIE_CLUSTER_PEER_LINK_ACTIVE ||
      link->state == FLOWIE_CLUSTER_PEER_LINK_CLOSING) {
    salts_mutex_unlock(&link->mutex);
    return SALTS_EBUSY;
  }
  salts_mutex_unlock(&link->mutex);
  flowie_cluster_peer_link_fail_pending(link, SALTS_ECANCELED);
  mem_buffer_release(link->receive_buffer);
  deque_destroy(&link->queue);
  salts_mutex_destroy(&link->mutex);
  tstr_free(link->cluster_id);
  tstr_free(link->local_node_id);
  tstr_free(link->remote_node_id);
  free(link);
  return SALTS_OK;
}

static int flowie_cluster_peer_link_route_validate(flowie_cluster_peer_link_t *link,
                                                   const flowie_cluster_peer_frame_t *frame) {
  if (frame->cluster_id.len != tstr_len(link->cluster_id) ||
      memcmp(frame->cluster_id.data, link->cluster_id, frame->cluster_id.len) != 0 ||
      frame->source_node_id.len != tstr_len(link->local_node_id) ||
      memcmp(frame->source_node_id.data, link->local_node_id, frame->source_node_id.len) != 0 ||
      memcmp(frame->source_boot_id, link->local_boot_id, sizeof(link->local_boot_id)) != 0 ||
      !link->remote_node_id || frame->target_node_id.len != tstr_len(link->remote_node_id) ||
      memcmp(frame->target_node_id.data, link->remote_node_id, frame->target_node_id.len) != 0 ||
      memcmp(frame->target_boot_id, link->remote_boot_id, sizeof(link->remote_boot_id)) != 0) {
    return SALTS_EPROTO;
  }
  return SALTS_OK;
}

int flowie_cluster_peer_link_send(flowie_cluster_peer_link_t *link,
                                  const flowie_cluster_peer_frame_t *frame,
                                  flowie_cluster_peer_send_complete_fn complete,
                                  void *complete_user_data) {
  flowie_cluster_peer_send_t send;
  size_t bytes;
  int rc;
  if (!link || !frame) return SALTS_EINVAL;
  memset(&send, 0, sizeof(send));
  rc = flowie_cluster_peer_frame_encode(frame, link->max_payload_size, &send.bytes);
  if (rc != SALTS_OK) return rc;
  bytes = tstr_len(send.bytes);
  send.complete = complete;
  send.complete_user_data = complete_user_data;
  salts_mutex_lock(&link->mutex);
  if (link->state != FLOWIE_CLUSTER_PEER_LINK_ACTIVE) {
    rc = link->state == FLOWIE_CLUSTER_PEER_LINK_CLOSING ||
                 link->state == FLOWIE_CLUSTER_PEER_LINK_CLOSED
             ? SALTS_ECANCELED
             : SALTS_EBUSY;
  } else if (flowie_cluster_peer_link_route_validate(link, frame) != SALTS_OK) {
    rc = SALTS_EPROTO;
  } else if (link->pending_entries >= link->queue_entries_limit ||
             bytes > link->queue_bytes_limit - link->pending_bytes) {
    rc = SALTS_ENOSPC;
  } else {
    rc = flowie_stl_error(deque_push_back(&link->queue, &send));
    if (rc == SALTS_OK) {
      ++link->pending_entries;
      link->pending_bytes += bytes;
      rc = cnet_client_wake(link->network);
      if (rc != SALTS_OK) {
        flowie_cluster_peer_send_t rollback;
        (void)flowie_stl_error(deque_pop_back(&link->queue, &rollback));
        --link->pending_entries;
        link->pending_bytes -= bytes;
      }
    }
  }
  salts_mutex_unlock(&link->mutex);
  if (rc != SALTS_OK) tstr_free(send.bytes);
  return rc;
}

int flowie_cluster_peer_link_close(flowie_cluster_peer_link_t *link) {
  cnet_client *network = NULL;
  cnet_connection connection = {0};
  int rc = SALTS_OK;
  if (!link) return SALTS_EINVAL;
  salts_mutex_lock(&link->mutex);
  if (link->state == FLOWIE_CLUSTER_PEER_LINK_CLOSED) {
    rc = SALTS_EALREADY;
  } else if (link->state == FLOWIE_CLUSTER_PEER_LINK_CREATED) {
    link->state = FLOWIE_CLUSTER_PEER_LINK_CLOSED;
  } else {
    link->state = FLOWIE_CLUSTER_PEER_LINK_CLOSING;
    network = link->network;
    connection = link->connection;
  }
  salts_mutex_unlock(&link->mutex);
  if (network != NULL && connection.slot != 0u) {
    rc = cnet_close(network, connection);
    if (rc == SALTS_EALREADY) rc = SALTS_OK;
    (void)cnet_client_wake(network);
  }
  return rc;
}

flowie_cluster_peer_link_state_t flowie_cluster_peer_link_state(flowie_cluster_peer_link_t *link) {
  flowie_cluster_peer_link_state_t state;
  if (!link) return FLOWIE_CLUSTER_PEER_LINK_CLOSED;
  salts_mutex_lock(&link->mutex);
  state = link->state;
  salts_mutex_unlock(&link->mutex);
  return state;
}

int flowie_cluster_peer_link_pending(flowie_cluster_peer_link_t *link, size_t *entries,
                                     size_t *bytes) {
  if (!link || !entries || !bytes) return SALTS_EINVAL;
  salts_mutex_lock(&link->mutex);
  *entries = link->pending_entries;
  *bytes = link->pending_bytes;
  salts_mutex_unlock(&link->mutex);
  return SALTS_OK;
}

static int flowie_cluster_peer_link_decode_buffer(flowie_cluster_peer_link_t *link,
                                                  flowie_cluster_peer_frame_t *out) {
  size_t consumed = 0u;
  size_t remaining;
  int rc = flowie_cluster_peer_frame_decode(mem_buffer_const_data(link->receive_buffer),
                                            mem_buffer_used(link->receive_buffer),
                                            link->max_payload_size, out, &consumed);
  if (rc != SALTS_OK) return rc;
  remaining = mem_buffer_used(link->receive_buffer) - consumed;
  if (remaining != 0u)
    memmove(mem_buffer_data(link->receive_buffer),
            mem_buffer_const_data(link->receive_buffer) + consumed, remaining);
  mem_set_used(link->receive_buffer, remaining);
  return SALTS_OK;
}

static int flowie_cluster_peer_link_receive_next(flowie_cluster_peer_link_t *link,
                                                  flowie_cluster_peer_frame_t *out) {
  for (;;) {
    size_t events = 0u;
    int rc = flowie_cluster_peer_link_decode_buffer(link, out);
    if (rc != FLOWIE_CLUSTER_PEER_INCOMPLETE) return rc;
    if (link->transport_status != SALTS_OK) return link->transport_status;
    if (link->transport_terminal) return SALTS_ECONNRESET;
    if (!link->receive_pending) {
      rc = cnet_receive(link->network, link->connection, 1u);
      if (rc != SALTS_OK) return rc;
      link->receive_pending = 1;
    }
    rc = cnet_client_poll(link->network, FLOWIE_CLUSTER_PEER_POLL_SLICE_MS, &events);
    if (rc != SALTS_OK) return rc;
    salts_mutex_lock(&link->mutex);
    if (link->state == FLOWIE_CLUSTER_PEER_LINK_CLOSING || link->pending_entries != 0u)
      rc = SALTS_EINTR;
    salts_mutex_unlock(&link->mutex);
    if (rc != SALTS_OK) return rc;
  }
}

static void flowie_cluster_peer_link_control_frame(flowie_cluster_peer_link_t *link,
                                                   flowie_cluster_peer_frame_kind_t kind,
                                                   vstr target_node_id,
                                                   const uint8_t *target_boot_id, vstr payload,
                                                   flowie_cluster_peer_frame_t *frame) {
  *frame = (flowie_cluster_peer_frame_t)FLOWIE_CLUSTER_PEER_FRAME_INIT;
  frame->kind = kind;
  frame->cluster_id = tstr_to_v(link->cluster_id);
  frame->source_node_id = tstr_to_v(link->local_node_id);
  frame->target_node_id = target_node_id;
  frame->payload = payload;
  memcpy(frame->source_boot_id, link->local_boot_id, sizeof(frame->source_boot_id));
  memcpy(frame->target_boot_id, target_boot_id, sizeof(frame->target_boot_id));
}

static int flowie_cluster_peer_link_send_bytes(flowie_cluster_peer_link_t *link, const void *data,
                                               size_t size) {
  int rc;
  if (link == NULL || link->network == NULL || data == NULL || size == 0u)
    return SALTS_EINVAL;
  link->send_pending = 1;
  link->expected_send_size = size;
  rc = cnet_send(link->network, link->connection, data, size);
  if (rc != SALTS_OK) {
    link->send_pending = 0;
    link->expected_send_size = 0u;
  }
  while (rc == SALTS_OK && link->send_pending && !link->transport_terminal &&
         link->transport_status == SALTS_OK) {
    size_t events = 0u;
    rc = cnet_client_poll(link->network, FLOWIE_CLUSTER_PEER_POLL_SLICE_MS, &events);
  }
  if (rc == SALTS_OK && link->transport_status != SALTS_OK) rc = link->transport_status;
  if (rc == SALTS_OK && link->transport_terminal) rc = SALTS_ECONNRESET;
  return rc;
}

static int flowie_cluster_peer_link_send_direct(flowie_cluster_peer_link_t *link,
                                                 const flowie_cluster_peer_frame_t *frame) {
  tstr encoded = NULL;
  int rc = flowie_cluster_peer_frame_encode(frame, link->max_payload_size, &encoded);
  if (rc == SALTS_OK)
    rc = flowie_cluster_peer_link_send_bytes(link, encoded, tstr_len(encoded));
  tstr_free(encoded);
  return rc;
}

static int flowie_cluster_peer_link_authorize(flowie_cluster_peer_link_t *link,
                                              const flowie_cluster_peer_frame_t *frame,
                                              const char *certificate_sha256) {
  return link->authorize(link->user_data, frame->source_node_id, frame->source_boot_id,
                         certificate_sha256);
}

static int flowie_cluster_peer_link_handshake_initiator(flowie_cluster_peer_link_t *link,
                                                        vstr channel_binding,
                                                        const char *certificate_sha256) {
  flowie_cluster_peer_frame_t hello;
  flowie_cluster_peer_frame_t reply = FLOWIE_CLUSTER_PEER_FRAME_INIT;
  int rc;
  flowie_cluster_peer_link_control_frame(link, FLOWIE_CLUSTER_PEER_FRAME_HELLO,
                                         tstr_to_v(link->remote_node_id), link->remote_boot_id,
                                         channel_binding, &hello);
  rc = flowie_cluster_peer_link_send_direct(link, &hello);
  do {
    if (rc == SALTS_OK) rc = flowie_cluster_peer_link_receive_next(link, &reply);
    if (rc == SALTS_EINTR &&
        flowie_cluster_peer_link_state(link) == FLOWIE_CLUSTER_PEER_LINK_CLOSING)
      rc = SALTS_ECANCELED;
  } while (rc == SALTS_EINTR);
  if (rc == SALTS_OK && reply.kind != FLOWIE_CLUSTER_PEER_FRAME_HELLO_ACK) rc = SALTS_EPROTO;
  if (rc == SALTS_OK)
    rc = flowie_cluster_peer_frame_require_target(
        &reply, tstr_to_v(link->cluster_id), tstr_to_v(link->local_node_id), link->local_boot_id);
  if (rc == SALTS_OK &&
      (reply.source_node_id.len != tstr_len(link->remote_node_id) ||
       memcmp(reply.source_node_id.data, link->remote_node_id, reply.source_node_id.len) != 0 ||
       memcmp(reply.source_boot_id, link->remote_boot_id, sizeof(link->remote_boot_id)) != 0 ||
       !vstr_eq(reply.payload, channel_binding))) {
    rc = SALTS_EPROTO;
  }
  if (rc == SALTS_OK) rc = flowie_cluster_peer_link_authorize(link, &reply, certificate_sha256);
  flowie_cluster_peer_frame_cleanup(&reply);
  return rc;
}

static int flowie_cluster_peer_link_handshake_responder(flowie_cluster_peer_link_t *link,
                                                        vstr channel_binding,
                                                        const char *certificate_sha256) {
  flowie_cluster_peer_frame_t hello = FLOWIE_CLUSTER_PEER_FRAME_INIT;
  flowie_cluster_peer_frame_t reply;
  int rc;
  do {
    rc = flowie_cluster_peer_link_receive_next(link, &hello);
    if (rc == SALTS_EINTR &&
        flowie_cluster_peer_link_state(link) == FLOWIE_CLUSTER_PEER_LINK_CLOSING)
      rc = SALTS_ECANCELED;
  } while (rc == SALTS_EINTR);
  if (rc == SALTS_OK && hello.kind != FLOWIE_CLUSTER_PEER_FRAME_HELLO) rc = SALTS_EPROTO;
  if (rc == SALTS_OK)
    rc = flowie_cluster_peer_frame_require_target(
        &hello, tstr_to_v(link->cluster_id), tstr_to_v(link->local_node_id), link->local_boot_id);
  if (rc == SALTS_OK && !vstr_eq(hello.payload, channel_binding)) rc = SALTS_EPROTO;
  if (rc == SALTS_OK) rc = flowie_cluster_peer_link_authorize(link, &hello, certificate_sha256);
  if (rc == SALTS_OK) {
    link->remote_node_id = tstr_from_v(hello.source_node_id);
    if (!link->remote_node_id) {
      rc = SALTS_ENOMEM;
    } else {
      memcpy(link->remote_boot_id, hello.source_boot_id, sizeof(link->remote_boot_id));
    }
  }
  if (rc == SALTS_OK) {
    flowie_cluster_peer_link_control_frame(link, FLOWIE_CLUSTER_PEER_FRAME_HELLO_ACK,
                                           tstr_to_v(link->remote_node_id), link->remote_boot_id,
                                           channel_binding, &reply);
    rc = flowie_cluster_peer_link_send_direct(link, &reply);
  }
  flowie_cluster_peer_frame_cleanup(&hello);
  return rc;
}

static int flowie_cluster_peer_link_drain_one(flowie_cluster_peer_link_t *link) {
  flowie_cluster_peer_send_t send;
  size_t bytes;
  int rc = flowie_cluster_peer_link_pop(link, &send);
  if (rc != SALTS_OK) return rc;
  bytes = tstr_len(send.bytes);
  rc = flowie_cluster_peer_link_send_bytes(link, send.bytes, bytes);
  flowie_cluster_peer_link_complete_account(link, bytes);
  flowie_cluster_peer_send_complete(&send, rc);
  return rc;
}

static int flowie_cluster_peer_link_shutdown_transport(flowie_cluster_peer_link_t *link) {
  int status = SALTS_OK;
  if (!link->transport_terminal) {
    status = cnet_close(link->network, link->connection);
    if (status == SALTS_EALREADY) status = SALTS_OK;
  }
  while (status == SALTS_OK && !link->transport_terminal) {
    size_t events = 0u;
    status = cnet_client_poll(link->network, FLOWIE_CLUSTER_PEER_POLL_SLICE_MS, &events);
  }
  return status;
}

int flowie_cluster_peer_link_run(flowie_cluster_peer_link_t *link,
                                 cnet_connection connected_tls_connection) {
  uint8_t channel_binding[CNET_TLS_CHANNEL_BINDING_BYTES];
  char certificate_sha256[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY];
  int rc;
  if (!link || connected_tls_connection.slot == 0u ||
      connected_tls_connection.generation == 0u)
    return SALTS_EINVAL;
  salts_mutex_lock(&link->mutex);
  if (link->state != FLOWIE_CLUSTER_PEER_LINK_CREATED || link->network == NULL ||
      (link->connection.slot != 0u &&
       !flowie_cluster_peer_connection_matches(link, connected_tls_connection))) {
    salts_mutex_unlock(&link->mutex);
    return SALTS_EBUSY;
  }
  link->state = FLOWIE_CLUSTER_PEER_LINK_HANDSHAKING;
  link->connection = connected_tls_connection;
  salts_mutex_unlock(&link->mutex);
  while (!link->transport_connected && !link->transport_terminal &&
         link->transport_status == SALTS_OK) {
    size_t events = 0u;
    rc = cnet_client_poll(link->network, FLOWIE_CLUSTER_PEER_POLL_SLICE_MS, &events);
    if (rc != SALTS_OK) break;
  }
  if (link->transport_status != SALTS_OK) rc = link->transport_status;
  else if (!link->transport_connected) rc = SALTS_ECONNRESET;
  else rc = cnet_tls_peer_certificate_sha256(link->network, link->connection,
                                             certificate_sha256);
  if (rc == SALTS_OK)
    rc = cnet_tls_export_channel_binding(link->network, link->connection, channel_binding);
  if (rc == SALTS_OK) {
    vstr binding = vstr_from_buf((const char *)channel_binding, sizeof(channel_binding));
    rc = link->role == FLOWIE_CLUSTER_PEER_ROLE_INITIATOR
             ? flowie_cluster_peer_link_handshake_initiator(link, binding, certificate_sha256)
             : flowie_cluster_peer_link_handshake_responder(link, binding, certificate_sha256);
  }
  salts_mutex_lock(&link->mutex);
  if (rc == SALTS_OK && link->state == FLOWIE_CLUSTER_PEER_LINK_HANDSHAKING)
    link->state = FLOWIE_CLUSTER_PEER_LINK_ACTIVE;
  salts_mutex_unlock(&link->mutex);

  if (rc == SALTS_OK && link->active)
    rc = link->active(link->user_data, link, tstr_to_v(link->remote_node_id), link->remote_boot_id);

  while (rc == SALTS_OK) {
    flowie_cluster_peer_frame_t frame = FLOWIE_CLUSTER_PEER_FRAME_INIT;
    flowie_cluster_peer_link_state_t state;
    size_t pending;
    salts_mutex_lock(&link->mutex);
    state = link->state;
    pending = link->pending_entries;
    salts_mutex_unlock(&link->mutex);
    if (pending != 0u) {
      rc = flowie_cluster_peer_link_drain_one(link);
      continue;
    }
    if (state == FLOWIE_CLUSTER_PEER_LINK_CLOSING) break;
    rc = flowie_cluster_peer_link_receive_next(link, &frame);
    if (rc == SALTS_EINTR) {
      rc = SALTS_OK;
      continue;
    }
    if (rc == SALTS_OK)
      rc = flowie_cluster_peer_frame_require_target(
          &frame, tstr_to_v(link->cluster_id), tstr_to_v(link->local_node_id), link->local_boot_id);
    if (rc == SALTS_OK &&
        (frame.source_node_id.len != tstr_len(link->remote_node_id) ||
         memcmp(frame.source_node_id.data, link->remote_node_id, frame.source_node_id.len) != 0 ||
         memcmp(frame.source_boot_id, link->remote_boot_id, sizeof(link->remote_boot_id)) != 0)) {
      rc = SALTS_EPROTO;
    }
    if (rc == SALTS_OK) rc = link->receive(link->user_data, &frame);
    flowie_cluster_peer_frame_cleanup(&frame);
  }

  {
    const int shutdown_status = flowie_cluster_peer_link_shutdown_transport(link);
    if (rc == SALTS_OK && shutdown_status != SALTS_OK) rc = shutdown_status;
  }
  if (rc != SALTS_OK) {
    salts_mutex_lock(&link->mutex);
    link->state = FLOWIE_CLUSTER_PEER_LINK_CLOSING;
    salts_mutex_unlock(&link->mutex);
    flowie_cluster_peer_link_fail_pending(link, rc);
  }
  salts_mutex_lock(&link->mutex);
  link->state = FLOWIE_CLUSTER_PEER_LINK_CLOSED;
  salts_mutex_unlock(&link->mutex);
  return rc;
}

static int flowie_cluster_peer_tls_paths_validate(const void *profile, const char *ca_file,
                                                   const char *cert_file, const char *key_file) {
  return !profile || !ca_file || !*ca_file || !cert_file || !*cert_file || !key_file || !*key_file
             ? SALTS_EINVAL
             : SALTS_OK;
}

int flowie_cluster_peer_tls_client_configure(cnet_tls_client *client, const char *ca_file,
                                              const char *cert_file, const char *key_file,
                                              const char *key_password,
                                              const char *server_name) {
  cnet_tls_client_config config;
  int rc = flowie_cluster_peer_tls_paths_validate(client, ca_file, cert_file, key_file);
  if (rc == SALTS_OK && (!server_name || !*server_name)) rc = SALTS_EINVAL;
  if (rc != SALTS_OK) return rc;
  memset(&config, 0, sizeof(config));
  config.size = sizeof(config);
  config.ca_file = ca_file;
  config.cert_file = cert_file;
  config.key_file = key_file;
  config.key_password = key_password;
  config.server_name = server_name;
  return cnet_tls_client_init(client, &config);
}

int flowie_cluster_peer_tls_server_configure(cnet_tls_server *server, const char *ca_file,
                                              const char *cert_file, const char *key_file,
                                              const char *key_password) {
  cnet_tls_server_config config;
  int rc = flowie_cluster_peer_tls_paths_validate(server, ca_file, cert_file, key_file);
  if (rc != SALTS_OK) return rc;
  memset(&config, 0, sizeof(config));
  config.size = sizeof(config);
  config.ca_file = ca_file;
  config.cert_file = cert_file;
  config.key_file = key_file;
  config.key_password = key_password;
  config.client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED;
  return cnet_tls_server_init(server, &config);
}
