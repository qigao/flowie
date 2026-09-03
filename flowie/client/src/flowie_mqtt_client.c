#include "flowie_mqtt_client.h"

#include "flowie_stl_error_internal.h"

#include <cstl.h>
#include <cstl.h>
#include <cstl.h>
#include <cstl.h>

#include <chttp/chttp.h>
#include <cnet/cnet.h>
#include <cnet/websocket.h>
#include "flowie_mqtt_protocol.h"
#include "monocypher.h"
#include "salts_bytes.h"
#include "salts_error.h"
#include "salts_str.h"
#include "salts_thread.h"

#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define FLOWIE_MQTT_CLIENT_TLS_STRING_LIMIT 4096u
#define FLOWIE_MQTT_CLIENT_IO_COMMAND_CAPACITY 8u
#define FLOWIE_MQTT_CLIENT_IO_REQUEST_CAPACITY 4u
#define FLOWIE_MQTT_CLIENT_IO_EVENT_CAPACITY 8u
#define FLOWIE_MQTT_CLIENT_IO_POLL_SLICE_MS 10u
#define FLOWIE_MQTT_CLIENT_HANDSHAKE_HEADER_BYTES 8192u
#define FLOWIE_MQTT_CLIENT_DEFAULT_STREAM_RECV_BUFFER_SIZE 4096u

typedef enum flowie_mqtt_client_state_e {
  FLOWIE_MQTT_CLIENT_DISCONNECTED = 0,
  FLOWIE_MQTT_CLIENT_TRANSPORT_CONNECTED,
  FLOWIE_MQTT_CLIENT_CONNECTED
} flowie_mqtt_client_state_t;

typedef enum flowie_mqtt_client_command_type_e {
  FLOWIE_MQTT_CLIENT_COMMAND_CONNECT = 1,
  FLOWIE_MQTT_CLIENT_COMMAND_PUBLISH,
  FLOWIE_MQTT_CLIENT_COMMAND_SUBSCRIBE,
  FLOWIE_MQTT_CLIENT_COMMAND_UNSUBSCRIBE,
  FLOWIE_MQTT_CLIENT_COMMAND_PING,
  FLOWIE_MQTT_CLIENT_COMMAND_AUTH,
  FLOWIE_MQTT_CLIENT_COMMAND_DISCONNECT
} flowie_mqtt_client_command_type_t;

typedef struct flowie_mqtt_client_command_s {
  flowie_mqtt_client_command_type_t type;
  flowie_mqtt_client_completion_fn completion;
  void *user_data;
  uint8_t *owned_bytes;
  size_t owned_size;
  int sensitive;
  union {
    flowie_mqtt_connect_packet_t connect;
    flowie_mqtt_publish_packet_t publish;
    flowie_mqtt_subscribe_packet_t subscribe;
    flowie_mqtt_unsubscribe_packet_t unsubscribe;
    struct {
      uint8_t reason_code;
      flowie_mqtt_span_t properties;
    } control;
  } packet;
} flowie_mqtt_client_command_t;

struct flowie_mqtt_client_s {
  cnet_client network;
  cnet_connection network_connection;
  chttp_websocket_client websocket;
  chttp_tls_profile websocket_tls;
  flowie_mqtt_client_transport_t transport;
  flowie_mqtt_client_state_t state;
  flowie_mqtt_version_t selected_version;
  flowie_mqtt_version_t version;
  tstr host;
  tstr path;
  tstr tls_ca_file;
  tstr tls_cert_file;
  tstr tls_key_file;
  tstr tls_key_password;
  tstr auth_method;
  int tls_configured;
  int port;
  uint64_t timeout_ms;
  size_t max_packet_size;
  size_t outbound_max_packet_size;
  size_t max_inbound_qos2;
  size_t stream_recv_buffer_bytes;
  size_t socket_recv_buffer_bytes;
  size_t socket_send_buffer_bytes;
  uint16_t server_receive_maximum;
  uint16_t server_topic_alias_maximum;
  uint16_t server_keep_alive;
  uint8_t server_maximum_qos;
  uint8_t server_retain_available;
  flowie_mqtt_client_topic_handler_t *topic_handlers;
  size_t topic_handler_count;
  flowie_mqtt_client_completion_fn on_connect;
  flowie_mqtt_client_completion_fn on_publish;
  flowie_mqtt_client_completion_fn on_subscribe;
  flowie_mqtt_client_completion_fn on_unsubscribe;
  flowie_mqtt_client_completion_fn on_ping;
  flowie_mqtt_client_auth_challenge_fn on_auth_challenge;
  flowie_mqtt_client_completion_fn on_auth;
  flowie_mqtt_client_completion_fn on_disconnect;
  flowie_mqtt_client_error_fn on_error;
  void *user_data;
  int resilience_enabled;
  uint64_t reconnect_initial_delay_ms;
  uint64_t reconnect_max_delay_ms;
  uint32_t reconnect_max_attempts;
  flowie_mqtt_client_refresh_connect_fn refresh_connect;
  flowie_mqtt_client_reconnect_fn on_reconnect;
  flowie_mqtt_client_command_t *reconnect_connect;
  uint64_t reconnect_delay_ms;
  uint64_t reconnect_deadline_ms;
  uint32_t reconnect_attempt;
  int reconnect_pending;
  uint8_t disconnect_reason;
  int disconnect_reason_valid;
  tstr send_buffer;
  salts_bytes_t framing;
  char *recv_data;
  size_t recv_size;
  size_t recv_offset;
  size_t pending_packet_size;
  uint16_t next_packet_id;
  hash_set_t inbound_qos2;
  int framing_initialized;
  int qos2_initialized;
  int busy;
  int callback_active;
  int command_queue_initialized;
  int sync_initialized;
  int worker_started;
  int stopping;
  int version_locked;
  size_t command_queue_capacity;
  size_t command_queue_max_bytes;
  size_t command_queue_bytes;
  deque_t commands;
  salts_mutex_t command_mutex;
  salts_cond_t command_changed;
  salts_thread_t worker;
  int network_initialized;
  int websocket_initialized;
  int websocket_tls_initialized;
  int network_connected;
  int network_terminal;
  int network_receive_ready;
  int network_send_ready;
  int network_status;
  atomic_int public_connected;
};

static SALTS_THREAD_LOCAL flowie_mqtt_client_t *flowie_mqtt_client_current;

static int flowie_mqtt_client_connect_operation(flowie_mqtt_client_t *client,
                                                const flowie_mqtt_connect_packet_t *packet,
                                                flowie_mqtt_control_packet_view_t *connack);
static int flowie_mqtt_client_publish_operation(flowie_mqtt_client_t *client,
                                                const flowie_mqtt_publish_packet_t *packet,
                                                flowie_mqtt_control_packet_view_t *ack);
static int flowie_mqtt_client_subscribe_operation(flowie_mqtt_client_t *client,
                                                  const flowie_mqtt_subscribe_packet_t *packet,
                                                  flowie_mqtt_control_packet_view_t *suback);
static int flowie_mqtt_client_unsubscribe_operation(flowie_mqtt_client_t *client,
                                                    const flowie_mqtt_unsubscribe_packet_t *packet,
                                                    flowie_mqtt_control_packet_view_t *unsuback);
static int flowie_mqtt_client_ping_operation(flowie_mqtt_client_t *client);
static int flowie_mqtt_client_auth_operation(flowie_mqtt_client_t *client,
                                             flowie_mqtt_span_t properties,
                                             flowie_mqtt_control_packet_view_t *auth);
static int flowie_mqtt_client_disconnect_operation(flowie_mqtt_client_t *client,
                                                   uint8_t reason_code,
                                                   flowie_mqtt_span_t properties);

static int flowie_mqtt_client_parse_status(int rc) {
  switch (rc) {
  case FLOWIE_MQTT_PARSE_OK:
    return SALTS_OK;
  case FLOWIE_MQTT_PARSE_NO_MEMORY:
    return SALTS_ENOMEM;
  case FLOWIE_MQTT_PARSE_TOO_LARGE:
    return SALTS_EMSGSIZE;
  case FLOWIE_MQTT_PARSE_INVALID_ARGUMENT:
    return SALTS_EINVAL;
  default:
    return SALTS_EPROTO;
  }
}

static int flowie_mqtt_client_transport_valid(flowie_mqtt_client_transport_t transport) {
  return transport >= FLOWIE_MQTT_CLIENT_TRANSPORT_TCP &&
         transport <= FLOWIE_MQTT_CLIENT_TRANSPORT_WSS;
}

static native_io_backend_kind flowie_mqtt_client_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int flowie_mqtt_client_is_websocket(const flowie_mqtt_client_t *client);

static cnet_client_config flowie_mqtt_client_network_config(
    const flowie_mqtt_client_t *client) {
  size_t max_send_bytes = client->max_packet_size;
  size_t receive_buffer_bytes = client->stream_recv_buffer_bytes
                                    ? client->stream_recv_buffer_bytes
                                    : FLOWIE_MQTT_CLIENT_DEFAULT_STREAM_RECV_BUFFER_SIZE;
  cnet_client_config config;
  if (flowie_mqtt_client_is_websocket(client))
    max_send_bytes += CNET_WEBSOCKET_MAX_HEADER_BYTES;
  config = (cnet_client_config){.backend = flowie_mqtt_client_backend(),
                                .connection_capacity = 1u,
                                .command_capacity = FLOWIE_MQTT_CLIENT_IO_COMMAND_CAPACITY,
                                .request_capacity = FLOWIE_MQTT_CLIENT_IO_REQUEST_CAPACITY,
                                .completion_batch_capacity = FLOWIE_MQTT_CLIENT_IO_REQUEST_CAPACITY,
                                .event_capacity = FLOWIE_MQTT_CLIENT_IO_EVENT_CAPACITY,
                                .max_send_bytes = max_send_bytes,
                                .receive_buffer_bytes = receive_buffer_bytes,
                                .connect_timeout_ms = (uint32_t)client->timeout_ms,
                                .read_timeout_ms = (uint32_t)client->timeout_ms,
                                .write_timeout_ms = (uint32_t)client->timeout_ms};
  if (client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_TLS ||
      client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WSS) {
    config.tls_io_buffer_bytes = receive_buffer_bytes < CNET_TLS_MIN_IO_BUFFER_BYTES
                                     ? CNET_TLS_MIN_IO_BUFFER_BYTES
                                     : receive_buffer_bytes;
    config.tls_handshake_timeout_ms = (uint32_t)client->timeout_ms;
  }
  return config;
}

static cnet_stream_socket_options flowie_mqtt_client_socket_options(
    const flowie_mqtt_client_t *client) {
  cnet_stream_socket_options options = CNET_STREAM_SOCKET_OPTIONS_INIT;
  options.receive_buffer_bytes = client->socket_recv_buffer_bytes;
  options.send_buffer_bytes = client->socket_send_buffer_bytes;
  return options;
}

static int flowie_mqtt_client_is_websocket(const flowie_mqtt_client_t *client) {
  return client && (client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WS ||
                    client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WSS);
}

static uint32_t flowie_mqtt_client_poll_slice(uint64_t deadline_ms) {
  const uint64_t now = salts_monotonic_ms();
  const uint64_t remaining = deadline_ms > now ? deadline_ms - now : 0u;
  return (uint32_t)(remaining < FLOWIE_MQTT_CLIENT_IO_POLL_SLICE_MS
                        ? remaining
                        : FLOWIE_MQTT_CLIENT_IO_POLL_SLICE_MS);
}

static int flowie_mqtt_client_should_interrupt(flowie_mqtt_client_t *client) {
  int stopping;
  int queued;
  salts_mutex_lock(&client->command_mutex);
  stopping = client->stopping;
  queued = !deque_empty(&client->commands);
  salts_mutex_unlock(&client->command_mutex);
  if (stopping) return SALTS_ESHUTDOWN;
  return !client->busy && queued ? SALTS_EINTR : SALTS_OK;
}

static void flowie_mqtt_client_network_state(void *user, cnet_connection connection,
                                             cnet_connection_state state,
                                             const cnet_error *error) {
  flowie_mqtt_client_t *client = (flowie_mqtt_client_t *)user;
  if (!client || connection.slot != client->network_connection.slot ||
      connection.generation != client->network_connection.generation)
    return;
  if (state == CNET_CONNECTION_CONNECTED) {
    client->network_connected = 1;
    client->network_status = SALTS_OK;
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    client->network_connected = 0;
    client->network_terminal = 1;
    client->network_status = error ? error->status : SALTS_ECONNRESET;
  }
}

static void flowie_mqtt_client_network_receive(void *user, cnet_connection connection,
                                               const cnet_receive_view *view) {
  flowie_mqtt_client_t *client = (flowie_mqtt_client_t *)user;
  char *copy;
  if (!client || connection.slot != client->network_connection.slot ||
      connection.generation != client->network_connection.generation || !view ||
      view->kind != CNET_MESSAGE_BYTES || !view->data || view->size == 0u) {
    if (client) {
      client->network_status = SALTS_EPROTO;
      client->network_receive_ready = 1;
    }
    return;
  }
  copy = (char *)malloc(view->size);
  if (!copy) {
    client->network_status = SALTS_ENOMEM;
    client->network_receive_ready = 1;
    return;
  }
  memcpy(copy, view->data, view->size);
  free(client->recv_data);
  client->recv_data = copy;
  client->recv_size = view->size;
  client->recv_offset = 0u;
  client->network_status = SALTS_OK;
  client->network_receive_ready = 1;
}

static void flowie_mqtt_client_network_send(void *user, cnet_connection connection,
                                            size_t size) {
  flowie_mqtt_client_t *client = (flowie_mqtt_client_t *)user;
  (void)size;
  if (!client || connection.slot != client->network_connection.slot ||
      connection.generation != client->network_connection.generation)
    return;
  client->network_status = SALTS_OK;
  client->network_send_ready = 1;
}

static const flowie_mqtt_client_tls_config_t *
flowie_mqtt_client_tls_config(const flowie_mqtt_client_config_t *config) {
  return config ? &config->tls : NULL;
}

static int flowie_mqtt_client_tls_string_valid(const char *value) {
  size_t length;
  if (!value) return 1;
  length = strlen(value);
  return length > 0u && length <= FLOWIE_MQTT_CLIENT_TLS_STRING_LIMIT;
}

static int flowie_mqtt_client_config_validate(const flowie_mqtt_client_config_t *config) {
  const flowie_mqtt_client_tls_config_t *tls;
  size_t max_packet_size;
  if (!config || config->size != sizeof(*config) ||
      !config->host || config->host[0] == '\0' || config->port < 1 || config->port > 65535 ||
      !flowie_mqtt_client_transport_valid(config->transport))
    return SALTS_EINVAL;
  if ((config->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WS ||
       config->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WSS) &&
      config->path && config->path[0] != '/')
    return SALTS_EINVAL;
  max_packet_size = config->max_packet_size ? config->max_packet_size
                                            : FLOWIE_MQTT_CLIENT_DEFAULT_MAX_PACKET_SIZE;
  if (max_packet_size < 2u || max_packet_size > FLOWIE_MQTT_MAX_WIRE_PACKET_SIZE ||
      config->max_inbound_qos2 == 0u)
    return SALTS_EINVAL;
  if (config->command_queue_capacity == SIZE_MAX || config->command_queue_max_bytes == SIZE_MAX)
    return SALTS_EINVAL;
  tls = flowie_mqtt_client_tls_config(config);
  if (tls) {
    int has_cert = tls->cert_file != NULL;
    int has_key = tls->key_file != NULL;
    int has_tls_config = tls->ca_file || tls->cert_file || tls->key_file || tls->key_password;
    if ((has_tls_config && config->transport != FLOWIE_MQTT_CLIENT_TRANSPORT_TLS &&
         config->transport != FLOWIE_MQTT_CLIENT_TRANSPORT_WSS) ||
        has_cert != has_key || (tls->key_password && !has_key) ||
        !flowie_mqtt_client_tls_string_valid(tls->ca_file) ||
        !flowie_mqtt_client_tls_string_valid(tls->cert_file) ||
        !flowie_mqtt_client_tls_string_valid(tls->key_file) ||
        !flowie_mqtt_client_tls_string_valid(tls->key_password))
      return SALTS_EINVAL;
  }
  if (config->topic_handlers.count != 0u && !config->topic_handlers.data) return SALTS_EINVAL;
  if (config->socket_recv_buffer_bytes > (size_t)INT_MAX ||
      config->socket_send_buffer_bytes > (size_t)INT_MAX)
    return SALTS_ERANGE;
  if (config->timeout_ms > UINT32_MAX) return SALTS_ERANGE;
  if ((config->stream_recv_buffer_bytes != 0u &&
       config->stream_recv_buffer_bytes < FLOWIE_MQTT_CLIENT_MIN_STREAM_RECV_BUFFER_SIZE) ||
      config->stream_recv_buffer_bytes > FLOWIE_MQTT_CLIENT_MAX_STREAM_RECV_BUFFER_SIZE)
    return SALTS_ERANGE;
  for (size_t i = 0u; i < config->topic_handlers.count; ++i) {
    const flowie_mqtt_client_topic_handler_t *handler = &config->topic_handlers.data[i];
    if (!handler->on_message || !handler->filter.data || handler->filter.size == 0u ||
        !flowie_mqtt_topic_filter_validate(handler->filter))
      return SALTS_EINVAL;
    for (size_t j = 0u; j < i; ++j) {
      flowie_mqtt_span_t previous = config->topic_handlers.data[j].filter;
      if (previous.size == handler->filter.size &&
          memcmp(previous.data, handler->filter.data, previous.size) == 0)
        return SALTS_EINVAL;
    }
  }
  return SALTS_OK;
}

static int flowie_mqtt_client_resilience_validate(
    const flowie_mqtt_client_resilience_config_t *resilience) {
  uint64_t initial_delay_ms;
  uint64_t max_delay_ms;
  if (!resilience) return SALTS_OK;
  if (resilience->size != sizeof(*resilience)) return SALTS_EINVAL;
  initial_delay_ms = resilience->initial_delay_ms
                         ? resilience->initial_delay_ms
                         : FLOWIE_MQTT_CLIENT_DEFAULT_RECONNECT_INITIAL_DELAY_MS;
  max_delay_ms = resilience->max_delay_ms
                     ? resilience->max_delay_ms
                     : FLOWIE_MQTT_CLIENT_DEFAULT_RECONNECT_MAX_DELAY_MS;
  return initial_delay_ms <= max_delay_ms ? SALTS_OK : SALTS_EINVAL;
}

static int flowie_mqtt_client_auth_method_get(const flowie_mqtt_property_block_view_t *properties,
                                              flowie_mqtt_span_t *method) {
  flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
  flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
  int found = 0;
  int rc;
  if (!properties || !method) return SALTS_EINVAL;
  *method = (flowie_mqtt_span_t){0};
  if (properties->values.size == 0u) return SALTS_OK;
  rc = flowie_mqtt_property_iterator_init(properties, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) return flowie_mqtt_client_parse_status(rc);
  while ((rc = flowie_mqtt_property_iterator_next(&iterator, &property)) == FLOWIE_MQTT_PARSE_OK) {
    if (property.identifier != FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD) continue;
    if (found || property.value.size == 0u) return SALTS_EPROTO;
    *method = property.value;
    found = 1;
  }
  return rc == FLOWIE_MQTT_PARSE_NEED_MORE ? SALTS_OK : flowie_mqtt_client_parse_status(rc);
}

static int
flowie_mqtt_client_auth_method_matches(const flowie_mqtt_client_t *client,
                                       const flowie_mqtt_property_block_view_t *properties,
                                       int required) {
  flowie_mqtt_span_t method = {0};
  int rc;
  if (!client || !properties) return SALTS_EINVAL;
  rc = flowie_mqtt_client_auth_method_get(properties, &method);
  if (rc != SALTS_OK) return rc;
  if (method.size == 0u) return required ? SALTS_EPROTO : SALTS_OK;
  if (!client->auth_method || method.size != tstr_len(client->auth_method) ||
      memcmp(method.data, client->auth_method, method.size) != 0)
    return SALTS_EPROTO;
  return SALTS_OK;
}

static int flowie_mqtt_client_auth_method_select(flowie_mqtt_client_t *client,
                                                 flowie_mqtt_span_t properties) {
  flowie_mqtt_property_block_view_t block = FLOWIE_MQTT_PROPERTY_BLOCK_VIEW_INIT;
  flowie_mqtt_span_t method = {0};
  tstr selected = NULL;
  int rc;
  if (!client) return SALTS_EINVAL;
  block.values = properties;
  rc = flowie_mqtt_client_auth_method_get(&block, &method);
  if (rc != SALTS_OK) return rc;
  if (method.size != 0u) {
    selected = tstr_new_len(method.data, method.size);
    if (!selected) return SALTS_ENOMEM;
  }
  tstr_freep(&client->auth_method);
  client->auth_method = selected;
  return SALTS_OK;
}

static void flowie_mqtt_client_recv_release(flowie_mqtt_client_t *client) {
  if (!client || !client->recv_data) return;
  free(client->recv_data);
  client->recv_data = NULL;
  client->recv_size = 0u;
  client->recv_offset = 0u;
}

static void flowie_mqtt_client_transport_close(flowie_mqtt_client_t *client, int reset_framing) {
  if (!client) return;
  flowie_mqtt_client_recv_release(client);
  if (client->websocket_initialized) {
    const int close_status = chttp_websocket_client_close(
        &client->websocket, 1000u, NULL, 0u, (uint32_t)client->timeout_ms);
    (void)close_status;
    (void)chttp_websocket_client_destroy(&client->websocket, (uint32_t)client->timeout_ms);
    client->websocket_initialized = 0;
  }
  if (client->network_connection.slot != 0u && client->network_initialized) {
    uint64_t deadline = salts_monotonic_ms() + client->timeout_ms;
    if (!client->network_terminal) (void)cnet_close(&client->network, client->network_connection);
    while (!client->network_terminal && salts_monotonic_ms() < deadline) {
      size_t events = 0u;
      const uint32_t slice = flowie_mqtt_client_poll_slice(deadline);
      if (cnet_client_poll(&client->network, slice, &events) != SALTS_OK) break;
    }
    client->network_connection = (cnet_connection){0};
  }
  client->network_connected = 0;
  client->network_terminal = 0;
  client->network_receive_ready = 0;
  client->network_send_ready = 0;
  client->network_status = SALTS_OK;
  client->state = FLOWIE_MQTT_CLIENT_DISCONNECTED;
  atomic_store_explicit(&client->public_connected, 0, memory_order_release);
  client->version = FLOWIE_MQTT_VERSION_UNSPECIFIED;
  tstr_freep(&client->auth_method);
  client->outbound_max_packet_size = client->max_packet_size;
  client->server_receive_maximum = UINT16_MAX;
  client->server_topic_alias_maximum = 0u;
  client->server_keep_alive = 0u;
  client->server_maximum_qos = 2u;
  client->server_retain_available = 1u;
  client->pending_packet_size = 0u;
  if (reset_framing && client->framing_initialized) salts_bytes_reset(&client->framing);
  if (client->qos2_initialized) hash_set_clear(&client->inbound_qos2);
}

static int flowie_mqtt_client_begin(flowie_mqtt_client_t *client, int require_connected) {
  if (!client) return SALTS_EINVAL;
  if (client->busy || client->callback_active) return SALTS_EBUSY;
  if (flowie_mqtt_client_current != client) return SALTS_EBUSY;
  if (require_connected && client->state != FLOWIE_MQTT_CLIENT_CONNECTED) return SALTS_ENOTCONN;
  client->busy = 1;
  return SALTS_OK;
}

static void flowie_mqtt_client_end(flowie_mqtt_client_t *client) {
  if (client) client->busy = 0;
}

static int flowie_mqtt_client_ack_output_valid(const flowie_mqtt_control_packet_view_t *out) {
  return !out || (out->size >= sizeof(*out) && out->abi_version == FLOWIE_MQTT_PROTOCOL_ABI_V1);
}

static int flowie_mqtt_client_span_valid(flowie_mqtt_span_t span) {
  return span.size == 0u || span.data != NULL;
}

static int flowie_mqtt_client_size_add(size_t *total, size_t value) {
  if (!total || value > SIZE_MAX - *total) return SALTS_EMSGSIZE;
  *total += value;
  return SALTS_OK;
}

static int flowie_mqtt_client_size_array(size_t *total, size_t count, size_t elem_size) {
  if (count != 0u && elem_size > SIZE_MAX / count) return SALTS_EMSGSIZE;
  return flowie_mqtt_client_size_add(total, count * elem_size);
}

static void flowie_mqtt_client_copy_span(flowie_mqtt_span_t source, uint8_t **cursor,
                                         flowie_mqtt_span_t *out) {
  *out = source;
  if (source.size == 0u) {
    out->data = NULL;
    return;
  }
  memcpy(*cursor, source.data, source.size);
  out->data = *cursor;
  *cursor += source.size;
}

static int
flowie_mqtt_client_clone_topic_handlers(flowie_mqtt_client_t *client,
                                        const flowie_mqtt_client_topic_handler_map_t *map) {
  flowie_mqtt_client_topic_handler_t *handlers;
  size_t array_size = 0u;
  size_t total = 0u;
  uint8_t *cursor;
  int rc;
  if (!client || !map) return SALTS_EINVAL;
  if (map->count == 0u) return SALTS_OK;
  rc =
      flowie_mqtt_client_size_array(&total, map->count, sizeof(flowie_mqtt_client_topic_handler_t));
  if (rc != SALTS_OK) return rc;
  array_size = total;
  for (size_t i = 0u; i < map->count; ++i) {
    rc = flowie_mqtt_client_size_add(&total, map->data[i].filter.size);
    if (rc != SALTS_OK) return rc;
  }
  handlers = (flowie_mqtt_client_topic_handler_t *)malloc(total);
  if (!handlers) return SALTS_ENOMEM;
  memcpy(handlers, map->data, array_size);
  cursor = (uint8_t *)handlers + array_size;
  for (size_t i = 0u; i < map->count; ++i)
    flowie_mqtt_client_copy_span(map->data[i].filter, &cursor, &handlers[i].filter);
  client->topic_handlers = handlers;
  client->topic_handler_count = map->count;
  return SALTS_OK;
}

static flowie_mqtt_client_command_t *
flowie_mqtt_client_command_new(flowie_mqtt_client_command_type_t type,
                               flowie_mqtt_client_completion_fn completion, void *user_data) {
  flowie_mqtt_client_command_t *command;
  if (!completion) return NULL;
  command = (flowie_mqtt_client_command_t *)calloc(1, sizeof(*command));
  if (!command) return NULL;
  command->type = type;
  command->completion = completion;
  command->user_data = user_data;
  return command;
}

static void flowie_mqtt_client_command_destroy(flowie_mqtt_client_command_t *command) {
  if (!command) return;
  if (command->sensitive && command->owned_bytes)
    crypto_wipe(command->owned_bytes, command->owned_size);
  free(command->owned_bytes);
  free(command);
}

static int flowie_mqtt_client_command_allocate(flowie_mqtt_client_command_t *command, size_t size,
                                               uint8_t **cursor) {
  if (!command || !cursor) return SALTS_EINVAL;
  *cursor = NULL;
  if (size == 0u) return SALTS_OK;
  command->owned_bytes = (uint8_t *)malloc(size);
  if (!command->owned_bytes) return SALTS_ENOMEM;
  command->owned_size = size;
  *cursor = command->owned_bytes;
  return SALTS_OK;
}

static int flowie_mqtt_client_clone_connect(flowie_mqtt_client_command_t *command,
                                            const flowie_mqtt_connect_packet_t *packet) {
  flowie_mqtt_span_t *spans;
  const flowie_mqtt_span_t source[] = {
      packet->properties,   packet->client_id, packet->will_properties, packet->will_topic,
      packet->will_payload, packet->username,  packet->password};
  size_t total = 0u;
  uint8_t *cursor;
  int rc;
  command->sensitive = 1;
  if (packet->size < sizeof(*packet) || packet->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1)
    return SALTS_EINVAL;
  for (size_t i = 0u; i < sizeof(source) / sizeof(source[0]); ++i) {
    if (!flowie_mqtt_client_span_valid(source[i])) return SALTS_EINVAL;
    rc = flowie_mqtt_client_size_add(&total, source[i].size);
    if (rc != SALTS_OK) return rc;
  }
  rc = flowie_mqtt_client_command_allocate(command, total, &cursor);
  if (rc != SALTS_OK) return rc;
  command->packet.connect = *packet;
  spans = &command->packet.connect.properties;
  for (size_t i = 0u; i < sizeof(source) / sizeof(source[0]); ++i)
    flowie_mqtt_client_copy_span(source[i], &cursor, &spans[i]);
  return SALTS_OK;
}

static int flowie_mqtt_client_version_resolve(const flowie_mqtt_client_t *client,
                                              flowie_mqtt_version_t *version) {
  if (!client || !version) return SALTS_EINVAL;
  if (*version == FLOWIE_MQTT_VERSION_UNSPECIFIED) {
    *version = client->selected_version;
    return SALTS_OK;
  }
  if (!flowie_mqtt_version_is_supported(*version)) return SALTS_EINVAL;
  return *version == client->selected_version ? SALTS_OK : SALTS_EPROTO;
}

static int flowie_mqtt_client_reconnect_connect_replace(
    flowie_mqtt_client_t *client, const flowie_mqtt_connect_packet_t *packet) {
  flowie_mqtt_connect_packet_t resolved;
  flowie_mqtt_client_command_t *replacement;
  int rc;
  if (!client || !packet) return SALTS_EINVAL;
  resolved = *packet;
  rc = flowie_mqtt_client_version_resolve(client, &resolved.version);
  if (rc != SALTS_OK) return rc;
  replacement = (flowie_mqtt_client_command_t *)calloc(1, sizeof(*replacement));
  if (!replacement) return SALTS_ENOMEM;
  replacement->type = FLOWIE_MQTT_CLIENT_COMMAND_CONNECT;
  rc = flowie_mqtt_client_clone_connect(replacement, &resolved);
  if (rc != SALTS_OK) {
    flowie_mqtt_client_command_destroy(replacement);
    return rc;
  }
  flowie_mqtt_client_command_destroy(client->reconnect_connect);
  client->reconnect_connect = replacement;
  return SALTS_OK;
}

static void flowie_mqtt_client_reconnect_cancel(flowie_mqtt_client_t *client,
                                                int clear_connect) {
  if (!client) return;
  client->reconnect_pending = 0;
  client->reconnect_attempt = 0u;
  client->reconnect_delay_ms = client->reconnect_initial_delay_ms;
  client->reconnect_deadline_ms = 0u;
  if (clear_connect) {
    flowie_mqtt_client_command_destroy(client->reconnect_connect);
    client->reconnect_connect = NULL;
  }
}

static int flowie_mqtt_client_reconnect_reason(uint8_t reason_code) {
  return reason_code == UINT8_C(0x88) || reason_code == UINT8_C(0x89);
}

static int flowie_mqtt_client_refresh_reason(uint8_t reason_code) {
  return reason_code == UINT8_C(0x86) || reason_code == UINT8_C(0x87);
}

static int flowie_mqtt_client_reconnect_status(int status) {
  switch (status) {
  case SALTS_EOF:
  case SALTS_ECONNRESET:
  case SALTS_ECONNREFUSED:
  case SALTS_ETIMEDOUT:
  case SALTS_ENETDOWN:
  case SALTS_ENETUNREACH:
  case SALTS_EHOSTUNREACH:
  case SALTS_EIO:
    return 1;
  default:
    return 0;
  }
}

static void flowie_mqtt_client_reconnect_schedule(flowie_mqtt_client_t *client) {
  uint64_t now;
  if (!client || !client->resilience_enabled || !client->reconnect_connect) return;
  if (client->reconnect_max_attempts != 0u &&
      client->reconnect_attempt >= client->reconnect_max_attempts) {
    client->reconnect_pending = 0;
    return;
  }
  if (client->reconnect_delay_ms == 0u)
    client->reconnect_delay_ms = client->reconnect_initial_delay_ms;
  now = salts_monotonic_ms();
  client->reconnect_deadline_ms =
      client->reconnect_delay_ms > UINT64_MAX - now ? UINT64_MAX
                                                    : now + client->reconnect_delay_ms;
  client->reconnect_pending = 1;
}

static void flowie_mqtt_client_reconnect_backoff(flowie_mqtt_client_t *client) {
  uint64_t doubled;
  if (!client) return;
  doubled = client->reconnect_delay_ms > UINT64_MAX / 2u
                ? UINT64_MAX
                : client->reconnect_delay_ms * 2u;
  client->reconnect_delay_ms =
      doubled > client->reconnect_max_delay_ms ? client->reconnect_max_delay_ms : doubled;
}

static int flowie_mqtt_client_clone_publish(flowie_mqtt_client_command_t *command,
                                            const flowie_mqtt_publish_packet_t *packet) {
  const flowie_mqtt_span_t source[] = {packet->topic, packet->properties, packet->payload};
  flowie_mqtt_span_t *spans;
  size_t total = 0u;
  uint8_t *cursor;
  int rc;
  if (packet->size < sizeof(*packet) || packet->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      packet->packet_id != 0u)
    return SALTS_EINVAL;
  for (size_t i = 0u; i < sizeof(source) / sizeof(source[0]); ++i) {
    if (!flowie_mqtt_client_span_valid(source[i])) return SALTS_EINVAL;
    rc = flowie_mqtt_client_size_add(&total, source[i].size);
    if (rc != SALTS_OK) return rc;
  }
  rc = flowie_mqtt_client_command_allocate(command, total, &cursor);
  if (rc != SALTS_OK) return rc;
  command->packet.publish = *packet;
  spans = &command->packet.publish.topic;
  for (size_t i = 0u; i < sizeof(source) / sizeof(source[0]); ++i)
    flowie_mqtt_client_copy_span(source[i], &cursor, &spans[i]);
  return SALTS_OK;
}

static int flowie_mqtt_client_clone_publish_topic(flowie_mqtt_client_command_t *command,
                                                  flowie_mqtt_version_t version,
                                                  const flowie_mqtt_client_publish_topic_t *topic) {
  flowie_mqtt_publish_packet_t packet = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
  if (!topic) return SALTS_EINVAL;
  packet.version = version;
  packet.qos = topic->qos;
  packet.retain = topic->retain;
  packet.duplicate = topic->duplicate;
  packet.topic = topic->topic;
  packet.properties = topic->properties;
  packet.payload = topic->payload;
  return flowie_mqtt_client_clone_publish(command, &packet);
}

static int flowie_mqtt_client_clone_subscribe(flowie_mqtt_client_command_t *command,
                                              const flowie_mqtt_subscribe_packet_t *packet) {
  flowie_mqtt_subscription_t *subscriptions;
  size_t array_size;
  size_t total = 0u;
  uint8_t *cursor;
  int rc;
  if (packet->size < sizeof(*packet) || packet->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      packet->packet_id != 0u || !packet->subscriptions || packet->subscription_count == 0u ||
      !flowie_mqtt_client_span_valid(packet->properties))
    return SALTS_EINVAL;
  rc = flowie_mqtt_client_size_array(&total, packet->subscription_count,
                                     sizeof(flowie_mqtt_subscription_t));
  if (rc != SALTS_OK) return rc;
  array_size = total;
  rc = flowie_mqtt_client_size_add(&total, packet->properties.size);
  if (rc != SALTS_OK) return rc;
  for (size_t i = 0u; i < packet->subscription_count; ++i) {
    if (!flowie_mqtt_client_span_valid(packet->subscriptions[i].filter)) return SALTS_EINVAL;
    rc = flowie_mqtt_client_size_add(&total, packet->subscriptions[i].filter.size);
    if (rc != SALTS_OK) return rc;
  }
  rc = flowie_mqtt_client_command_allocate(command, total, &cursor);
  if (rc != SALTS_OK) return rc;
  subscriptions = (flowie_mqtt_subscription_t *)cursor;
  memcpy(subscriptions, packet->subscriptions, array_size);
  cursor += array_size;
  command->packet.subscribe = *packet;
  command->packet.subscribe.subscriptions = subscriptions;
  flowie_mqtt_client_copy_span(packet->properties, &cursor, &command->packet.subscribe.properties);
  for (size_t i = 0u; i < packet->subscription_count; ++i)
    flowie_mqtt_client_copy_span(packet->subscriptions[i].filter, &cursor,
                                 &subscriptions[i].filter);
  return SALTS_OK;
}

static int flowie_mqtt_client_clone_unsubscribe(flowie_mqtt_client_command_t *command,
                                                const flowie_mqtt_unsubscribe_packet_t *packet) {
  flowie_mqtt_span_t *filters;
  size_t array_size;
  size_t total = 0u;
  uint8_t *cursor;
  int rc;
  if (packet->size < sizeof(*packet) || packet->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      packet->packet_id != 0u || !packet->filters || packet->filter_count == 0u ||
      !flowie_mqtt_client_span_valid(packet->properties))
    return SALTS_EINVAL;
  rc = flowie_mqtt_client_size_array(&total, packet->filter_count, sizeof(flowie_mqtt_span_t));
  if (rc != SALTS_OK) return rc;
  array_size = total;
  rc = flowie_mqtt_client_size_add(&total, packet->properties.size);
  if (rc != SALTS_OK) return rc;
  for (size_t i = 0u; i < packet->filter_count; ++i) {
    if (!flowie_mqtt_client_span_valid(packet->filters[i])) return SALTS_EINVAL;
    rc = flowie_mqtt_client_size_add(&total, packet->filters[i].size);
    if (rc != SALTS_OK) return rc;
  }
  rc = flowie_mqtt_client_command_allocate(command, total, &cursor);
  if (rc != SALTS_OK) return rc;
  filters = (flowie_mqtt_span_t *)cursor;
  memcpy(filters, packet->filters, array_size);
  cursor += array_size;
  command->packet.unsubscribe = *packet;
  command->packet.unsubscribe.filters = filters;
  flowie_mqtt_client_copy_span(packet->properties, &cursor,
                               &command->packet.unsubscribe.properties);
  for (size_t i = 0u; i < packet->filter_count; ++i)
    flowie_mqtt_client_copy_span(packet->filters[i], &cursor, &filters[i]);
  return SALTS_OK;
}

static uint16_t flowie_mqtt_client_packet_id(flowie_mqtt_client_t *client) {
  uint16_t packet_id = client->next_packet_id;
  ++client->next_packet_id;
  if (client->next_packet_id == 0u) client->next_packet_id = 1u;
  return packet_id;
}

static int flowie_mqtt_client_send(flowie_mqtt_client_t *client, size_t written) {
  uint64_t deadline_ms;
  if (!client || written == 0u) return SALTS_EINVAL;
  if (written > client->outbound_max_packet_size) return SALTS_EMSGSIZE;
  if (flowie_mqtt_client_is_websocket(client)) {
    if (!client->websocket_initialized) return SALTS_ENOTCONN;
    return chttp_websocket_client_send_binary(&client->websocket, client->send_buffer, written,
                                               (uint32_t)client->timeout_ms);
  }
  if (!client->network_initialized || !client->network_connected) return SALTS_ENOTCONN;
  client->network_send_ready = 0;
  client->network_status = SALTS_OK;
  {
    int rc = cnet_send(&client->network, client->network_connection, client->send_buffer, written);
    if (rc != SALTS_OK) return rc;
  }
  deadline_ms = salts_monotonic_ms() + client->timeout_ms;
  while (!client->network_send_ready && !client->network_terminal) {
    size_t events = 0u;
    int rc = flowie_mqtt_client_should_interrupt(client);
    if (rc == SALTS_ESHUTDOWN) return rc;
    if (salts_monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
    rc = cnet_client_poll(&client->network, flowie_mqtt_client_poll_slice(deadline_ms), &events);
    if (rc != SALTS_OK) return rc;
  }
  return client->network_send_ready ? client->network_status
                                    : (client->network_status != SALTS_OK
                                           ? client->network_status
                                           : SALTS_ECONNRESET);
}

static int flowie_mqtt_client_transport_receive(flowie_mqtt_client_t *client) {
  const uint64_t deadline_ms = salts_monotonic_ms() + client->timeout_ms;
  int rc;
  if (!client) return SALTS_EINVAL;
  rc = flowie_mqtt_client_should_interrupt(client);
  if (rc != SALTS_OK) return rc;
  if (flowie_mqtt_client_is_websocket(client)) {
    if (!client->websocket_initialized) return SALTS_ENOTCONN;
    for (;;) {
      chttp_websocket_event event = {0};
      uint32_t slice;
      rc = flowie_mqtt_client_should_interrupt(client);
      if (rc != SALTS_OK) return rc;
      if (salts_monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
      slice = flowie_mqtt_client_poll_slice(deadline_ms);
      rc = chttp_websocket_client_receive(&client->websocket, slice, &event);
      if (rc == SALTS_ETIMEDOUT) continue;
      if (rc != SALTS_OK) return rc;
      if (event.kind == CHTTP_WEBSOCKET_EVENT_CLOSE) return SALTS_ECONNRESET;
      if (event.kind != CHTTP_WEBSOCKET_EVENT_MESSAGE) continue;
      if (event.message_type != CHTTP_WEBSOCKET_MESSAGE_BINARY || !event.data || event.size == 0u)
        return SALTS_EPROTO;
      client->recv_data = (char *)malloc(event.size);
      if (!client->recv_data) return SALTS_ENOMEM;
      memcpy(client->recv_data, event.data, event.size);
      client->recv_size = event.size;
      client->recv_offset = 0u;
      return SALTS_OK;
    }
  }
  if (!client->network_initialized || !client->network_connected) return SALTS_ENOTCONN;
  client->network_receive_ready = 0;
  client->network_status = SALTS_OK;
  rc = cnet_receive(&client->network, client->network_connection, 1u);
  if (rc != SALTS_OK) return rc;
  while (!client->network_receive_ready && !client->network_terminal) {
    size_t events = 0u;
    rc = flowie_mqtt_client_should_interrupt(client);
    if (rc != SALTS_OK) return rc;
    if (salts_monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
    rc = cnet_client_poll(&client->network, flowie_mqtt_client_poll_slice(deadline_ms), &events);
    if (rc != SALTS_OK) return rc;
  }
  return client->network_receive_ready ? client->network_status
                                       : (client->network_status != SALTS_OK
                                              ? client->network_status
                                              : SALTS_ECONNRESET);
}

static int flowie_mqtt_client_negotiate_connack(flowie_mqtt_client_t *client,
                                                const flowie_mqtt_control_packet_view_t *connack) {
  flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
  flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
  int rc;
  if (!client || !connack) return SALTS_EINVAL;
  client->outbound_max_packet_size = client->max_packet_size;
  client->server_receive_maximum = UINT16_MAX;
  client->server_topic_alias_maximum = 0u;
  client->server_keep_alive = 0u;
  client->server_maximum_qos = 2u;
  client->server_retain_available = 1u;
  if (client->version != FLOWIE_MQTT_VERSION_5 || connack->properties.values.size == 0u)
    return SALTS_OK;
  rc = flowie_mqtt_client_auth_method_matches(client, &connack->properties, 0);
  if (rc != SALTS_OK) return rc;
  rc = flowie_mqtt_property_iterator_init(&connack->properties, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  while ((rc = flowie_mqtt_property_iterator_next(&iterator, &property)) == FLOWIE_MQTT_PARSE_OK) {
    switch (property.identifier) {
    case FLOWIE_MQTT_PROPERTY_RECEIVE_MAXIMUM:
      client->server_receive_maximum = (uint16_t)property.integer;
      break;
    case FLOWIE_MQTT_PROPERTY_MAXIMUM_PACKET_SIZE:
      if ((size_t)property.integer < client->outbound_max_packet_size)
        client->outbound_max_packet_size = property.integer;
      break;
    case FLOWIE_MQTT_PROPERTY_TOPIC_ALIAS_MAXIMUM:
      client->server_topic_alias_maximum = (uint16_t)property.integer;
      break;
    case FLOWIE_MQTT_PROPERTY_SERVER_KEEP_ALIVE:
      client->server_keep_alive = (uint16_t)property.integer;
      break;
    case FLOWIE_MQTT_PROPERTY_MAXIMUM_QOS:
      client->server_maximum_qos = (uint8_t)property.integer;
      break;
    case FLOWIE_MQTT_PROPERTY_RETAIN_AVAILABLE:
      client->server_retain_available = (uint8_t)property.integer;
      break;
    default:
      break;
    }
  }
  return rc == FLOWIE_MQTT_PARSE_NEED_MORE ? SALTS_OK : SALTS_EPROTO;
}

static int
flowie_mqtt_client_publish_capabilities_validate(const flowie_mqtt_client_t *client,
                                                 const flowie_mqtt_publish_packet_t *packet) {
  flowie_mqtt_property_block_view_t block = FLOWIE_MQTT_PROPERTY_BLOCK_VIEW_INIT;
  flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
  flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
  int rc;
  if (!client || !packet) return SALTS_EINVAL;
  if (client->version != FLOWIE_MQTT_VERSION_5) return SALTS_OK;
  if (packet->qos > client->server_maximum_qos ||
      (packet->retain && !client->server_retain_available))
    return SALTS_ENOTSUP;
  if (packet->properties.size == 0u) return SALTS_OK;
  block.values = packet->properties;
  rc = flowie_mqtt_property_iterator_init(&block, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  while ((rc = flowie_mqtt_property_iterator_next(&iterator, &property)) == FLOWIE_MQTT_PARSE_OK) {
    if (property.identifier == FLOWIE_MQTT_PROPERTY_TOPIC_ALIAS &&
        (property.integer == 0u || property.integer > client->server_topic_alias_maximum))
      return SALTS_ENOTSUP;
  }
  return rc == FLOWIE_MQTT_PARSE_NEED_MORE ? SALTS_OK : SALTS_EPROTO;
}

static int flowie_mqtt_client_send_control(flowie_mqtt_client_t *client,
                                           flowie_mqtt_packet_type_t type, uint16_t packet_id,
                                           uint8_t reason_code) {
  flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  size_t written = 0u;
  int rc;
  packet.version = client->version;
  packet.type = type;
  packet.packet_id = packet_id;
  packet.reason_code = reason_code;
  rc = flowie_mqtt_control_packet_encode(&packet, (uint8_t *)client->send_buffer,
                                         client->outbound_max_packet_size, &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return flowie_mqtt_client_parse_status(rc);
  return flowie_mqtt_client_send(client, written);
}

static int flowie_mqtt_client_send_auth(flowie_mqtt_client_t *client, uint8_t reason_code,
                                        flowie_mqtt_span_t properties) {
  flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  size_t written = 0u;
  int rc;
  if (!client || client->version != FLOWIE_MQTT_VERSION_5 ||
      !flowie_mqtt_client_span_valid(properties))
    return SALTS_EINVAL;
  packet.version = FLOWIE_MQTT_VERSION_5;
  packet.type = FLOWIE_MQTT_PACKET_AUTH;
  packet.reason_code = reason_code;
  packet.properties = properties;
  rc = flowie_mqtt_control_packet_encode(&packet, (uint8_t *)client->send_buffer,
                                         client->outbound_max_packet_size, &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return flowie_mqtt_client_parse_status(rc);
  return flowie_mqtt_client_send(client, written);
}

static int flowie_mqtt_client_receive_packet(flowie_mqtt_client_t *client,
                                             flowie_mqtt_packet_view_t *out) {
  flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  if (!client || !out) return SALTS_EINVAL;
  if (client->pending_packet_size != 0u) {
    int rc = salts_bytes_consume(&client->framing, client->pending_packet_size);
    if (rc != SALTS_OK) return rc;
    client->pending_packet_size = 0u;
  }
  options.version = client->version;
  options.max_packet_size = client->max_packet_size;
  for (;;) {
    salts_bytes_view_t bytes;
    flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
    size_t consumed = 0u;
    int rc = salts_bytes_view(&client->framing, &bytes);
    if (rc != SALTS_OK) return rc;
    if (bytes.size != 0u) {
      rc = flowie_mqtt_packet_parse(bytes.data, bytes.size, &options, &packet, &consumed, NULL);
      if (rc == FLOWIE_MQTT_PARSE_OK) {
        if (consumed == 0u || consumed > bytes.size) return SALTS_EPROTO;
        client->pending_packet_size = consumed;
        *out = packet;
        return SALTS_OK;
      }
      if (rc != FLOWIE_MQTT_PARSE_NEED_MORE) return flowie_mqtt_client_parse_status(rc);
      if (bytes.size == client->max_packet_size) return SALTS_EMSGSIZE;
    }
    if (client->recv_data) {
      size_t remaining = client->recv_size - client->recv_offset;
      size_t available = salts_bytes_available(&client->framing);
      size_t chunk = remaining < available ? remaining : available;
      if (chunk == 0u) return SALTS_EMSGSIZE;
      rc = salts_bytes_append(&client->framing, client->recv_data + client->recv_offset,
                                    chunk);
      if (rc != SALTS_OK) return rc;
      client->recv_offset += chunk;
      if (client->recv_offset == client->recv_size) flowie_mqtt_client_recv_release(client);
      continue;
    }
    rc = flowie_mqtt_client_transport_receive(client);
    if (rc != SALTS_OK) return rc;
    client->recv_offset = 0u;
    if (!client->recv_data && client->recv_size == 0u) return SALTS_ECONNRESET;
    if (!client->recv_data || client->recv_size == 0u) {
      flowie_mqtt_client_recv_release(client);
      return SALTS_EPROTO;
    }
  }
}

static int flowie_mqtt_client_handle_publish(flowie_mqtt_client_t *client,
                                             const flowie_mqtt_packet_view_t *packet) {
  flowie_mqtt_publish_view_t publish = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
  size_t match_count = 0u;
  int rc = flowie_mqtt_publish_parse(packet, &publish);
  if (rc != FLOWIE_MQTT_PARSE_OK) return flowie_mqtt_client_parse_status(rc);
  if (publish.qos == 2u &&
      hash_set_contains(&client->inbound_qos2, &publish.packet_id)) {
    if (!publish.duplicate) return SALTS_EPROTO;
    return flowie_mqtt_client_send_control(client, FLOWIE_MQTT_PACKET_PUBREC, publish.packet_id,
                                           0u);
  }
  if (publish.qos == 2u) {
    if (hash_set_size(&client->inbound_qos2) >= client->max_inbound_qos2)
      return SALTS_ENOSPC;
    rc = flowie_stl_error(hash_set_add(&client->inbound_qos2, &publish.packet_id));
    if (rc != SALTS_OK) return rc;
  }
  client->callback_active = 1;
  for (size_t i = 0u; i < client->topic_handler_count; ++i) {
    int matched = 0;
    rc = flowie_mqtt_topic_matches(client->topic_handlers[i].filter, publish.topic, &matched);
    if (rc != FLOWIE_MQTT_PARSE_OK) {
      rc = flowie_mqtt_client_parse_status(rc);
      break;
    }
    if (!matched) continue;
    ++match_count;
    rc = client->topic_handlers[i].on_message(client, &publish, client->user_data);
    if (rc != SALTS_OK) break;
  }
  client->callback_active = 0;
  if (rc == SALTS_OK && match_count == 0u) rc = SALTS_ENOTSUP;
  if (rc != SALTS_OK) {
    if (publish.qos == 2u)
      (void)hash_set_remove(&client->inbound_qos2, &publish.packet_id);
    return rc;
  }
  if (publish.qos == 1u)
    return flowie_mqtt_client_send_control(client, FLOWIE_MQTT_PACKET_PUBACK, publish.packet_id,
                                           0u);
  if (publish.qos == 2u)
    return flowie_mqtt_client_send_control(client, FLOWIE_MQTT_PACKET_PUBREC, publish.packet_id,
                                           0u);
  return SALTS_OK;
}

static int flowie_mqtt_client_handle_pubrel(flowie_mqtt_client_t *client,
                                            const flowie_mqtt_packet_view_t *packet) {
  flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  uint8_t reason_code = 0u;
  int rc = flowie_mqtt_control_packet_parse(packet, &control);
  if (rc != FLOWIE_MQTT_PARSE_OK) return flowie_mqtt_client_parse_status(rc);
  if (hash_set_remove(&client->inbound_qos2, &control.packet_id) != STL_OK &&
      client->version == FLOWIE_MQTT_VERSION_5)
    reason_code = 0x92u;
  return flowie_mqtt_client_send_control(client, FLOWIE_MQTT_PACKET_PUBCOMP, control.packet_id,
                                         reason_code);
}

static int
flowie_mqtt_client_handle_auth_challenge(flowie_mqtt_client_t *client,
                                         const flowie_mqtt_packet_view_t *packet,
                                         flowie_mqtt_control_packet_view_t *challenge_out) {
  flowie_mqtt_control_packet_view_t challenge = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  flowie_mqtt_client_auth_response_t response = FLOWIE_MQTT_CLIENT_AUTH_RESPONSE_INIT;
  int rc;
  if (!client || !packet || packet->type != FLOWIE_MQTT_PACKET_AUTH ||
      client->version != FLOWIE_MQTT_VERSION_5)
    return SALTS_EINVAL;
  rc = flowie_mqtt_control_packet_parse(packet, &challenge);
  if (rc != FLOWIE_MQTT_PARSE_OK) return flowie_mqtt_client_parse_status(rc);
  if (challenge.reason_code != 0x18u) return SALTS_EPROTO;
  rc = flowie_mqtt_client_auth_method_matches(client, &challenge.properties, 1);
  if (rc != SALTS_OK) return rc;
  if (!client->on_auth_challenge) return SALTS_ENOTSUP;
  client->callback_active = 1;
  rc = client->on_auth_challenge(client, &challenge, &response, client->user_data);
  client->callback_active = 0;
  if (rc != SALTS_OK) return rc;
  if (response.size != sizeof(response) || response.reason_code != 0x18u ||
      !flowie_mqtt_client_span_valid(response.properties))
    return SALTS_EPROTO;
  {
    flowie_mqtt_property_block_view_t properties = FLOWIE_MQTT_PROPERTY_BLOCK_VIEW_INIT;
    properties.values = response.properties;
    rc = flowie_mqtt_client_auth_method_matches(client, &properties, 1);
    if (rc != SALTS_OK) return rc;
  }
  rc = flowie_mqtt_client_send_auth(client, response.reason_code, response.properties);
  if (rc == SALTS_OK && challenge_out) *challenge_out = challenge;
  return rc;
}

static int flowie_mqtt_client_handle_unsolicited(flowie_mqtt_client_t *client,
                                                 const flowie_mqtt_packet_view_t *packet,
                                                 int *handled) {
  if (!handled) return SALTS_EINVAL;
  *handled = 1;
  switch (packet->type) {
  case FLOWIE_MQTT_PACKET_PUBLISH:
    return flowie_mqtt_client_handle_publish(client, packet);
  case FLOWIE_MQTT_PACKET_PUBREL:
    return flowie_mqtt_client_handle_pubrel(client, packet);
  case FLOWIE_MQTT_PACKET_DISCONNECT: {
    flowie_mqtt_control_packet_view_t disconnect = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
    int rc = flowie_mqtt_control_packet_parse(packet, &disconnect);
    if (rc != FLOWIE_MQTT_PARSE_OK) return flowie_mqtt_client_parse_status(rc);
    client->disconnect_reason = disconnect.reason_code;
    client->disconnect_reason_valid = packet->version == FLOWIE_MQTT_VERSION_5;
    return SALTS_ECONNRESET;
  }
  case FLOWIE_MQTT_PACKET_AUTH:
    return SALTS_EPROTO;
  default:
    *handled = 0;
    return SALTS_OK;
  }
}

static int flowie_mqtt_client_wait_control(flowie_mqtt_client_t *client,
                                           flowie_mqtt_packet_type_t expected, uint16_t packet_id,
                                           flowie_mqtt_control_packet_view_t *out) {
  for (;;) {
    flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
    flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
    int handled = 0;
    int rc = flowie_mqtt_client_receive_packet(client, &packet);
    if (rc != SALTS_OK) return rc;
    if (expected == FLOWIE_MQTT_PACKET_CONNACK && packet.type == FLOWIE_MQTT_PACKET_AUTH) {
      rc = flowie_mqtt_client_handle_auth_challenge(client, &packet, NULL);
      if (rc != SALTS_OK) return rc;
      continue;
    }
    rc = flowie_mqtt_client_handle_unsolicited(client, &packet, &handled);
    if (rc != SALTS_OK) return rc;
    if (handled) continue;
    if (packet.type != expected) return SALTS_EPROTO;
    rc = flowie_mqtt_control_packet_parse(&packet, &control);
    if (rc != FLOWIE_MQTT_PARSE_OK) return flowie_mqtt_client_parse_status(rc);
    if (control.packet_id != packet_id) return SALTS_EPROTO;
    if (out) *out = control;
    return SALTS_OK;
  }
}

static void flowie_mqtt_client_complete(flowie_mqtt_client_t *client,
                                        flowie_mqtt_client_command_t *command, int status,
                                        const flowie_mqtt_control_packet_view_t *response) {
  client->callback_active = 1;
  command->completion(client, status, response, command->user_data);
  client->callback_active = 0;
}

static void flowie_mqtt_client_report_error(flowie_mqtt_client_t *client, int status) {
  if (!client->on_error) return;
  client->callback_active = 1;
  client->on_error(client, status, client->user_data);
  client->callback_active = 0;
}

static void flowie_mqtt_client_report_reconnect(
    flowie_mqtt_client_t *client, uint32_t attempt, int status,
    const flowie_mqtt_control_packet_view_t *response) {
  if (!client->on_reconnect) return;
  client->callback_active = 1;
  client->on_reconnect(client, attempt, status, response, client->user_data);
  client->callback_active = 0;
}

static int flowie_mqtt_client_refresh_reconnect(flowie_mqtt_client_t *client,
                                                uint8_t reason_code) {
  flowie_mqtt_connect_packet_t refreshed = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  int rc;
  if (!client || !client->refresh_connect || !client->reconnect_connect)
    return SALTS_ENOTSUP;
  client->callback_active = 1;
  rc = client->refresh_connect(client, reason_code,
                               &client->reconnect_connect->packet.connect, &refreshed,
                               client->user_data);
  client->callback_active = 0;
  if (rc != SALTS_OK) return rc;
  return flowie_mqtt_client_reconnect_connect_replace(client, &refreshed);
}

static void flowie_mqtt_client_reconnect_after_failure(flowie_mqtt_client_t *client,
                                                       int status, int increase_backoff) {
  uint8_t reason_code;
  int has_reason;
  int schedule = 0;
  if (!client || !client->resilience_enabled || !client->reconnect_connect) return;
  reason_code = client->disconnect_reason;
  has_reason = client->disconnect_reason_valid;
  client->disconnect_reason_valid = 0;
  if (has_reason && flowie_mqtt_client_refresh_reason(reason_code)) {
    int refresh_rc = flowie_mqtt_client_refresh_reconnect(client, reason_code);
    if (refresh_rc != SALTS_OK) {
      flowie_mqtt_client_reconnect_cancel(client, 1);
      flowie_mqtt_client_report_error(client, refresh_rc);
      return;
    }
    schedule = 1;
  } else if ((has_reason && reason_code == UINT8_C(0x89)) ||
             (!has_reason && flowie_mqtt_client_reconnect_status(status))) {
    schedule = 1;
  }
  if (!schedule) return;
  if (increase_backoff) flowie_mqtt_client_reconnect_backoff(client);
  flowie_mqtt_client_reconnect_schedule(client);
}

static int flowie_mqtt_client_command_pop(flowie_mqtt_client_t *client,
                                          flowie_mqtt_client_command_t **out) {
  int stopping;
  salts_mutex_lock(&client->command_mutex);
  stopping = client->stopping;
  if (!stopping) {
    if (deque_pop_front(&client->commands, out) != STL_OK) {
      *out = NULL;
    } else {
      client->command_queue_bytes -= sizeof(**out) + (*out)->owned_size;
    }
  }
  salts_mutex_unlock(&client->command_mutex);
  return stopping;
}

static void flowie_mqtt_client_cancel_commands(flowie_mqtt_client_t *client, int status) {
  for (;;) {
    flowie_mqtt_client_command_t *command = NULL;
    salts_mutex_lock(&client->command_mutex);
    (void)deque_pop_front(&client->commands, &command);
    if (command) client->command_queue_bytes -= sizeof(*command) + command->owned_size;
    salts_mutex_unlock(&client->command_mutex);
    if (!command) return;
    flowie_mqtt_client_complete(client, command, status, NULL);
    flowie_mqtt_client_command_destroy(command);
  }
}

static int flowie_mqtt_client_is_stopping(flowie_mqtt_client_t *client) {
  int stopping;
  salts_mutex_lock(&client->command_mutex);
  stopping = client->stopping;
  salts_mutex_unlock(&client->command_mutex);
  return stopping;
}

static void flowie_mqtt_client_run_reconnect(flowie_mqtt_client_t *client) {
  flowie_mqtt_control_packet_view_t response = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  const flowie_mqtt_control_packet_view_t *response_ptr = NULL;
  uint32_t attempt;
  int rc;
  if (!client || !client->reconnect_connect || flowie_mqtt_client_is_stopping(client)) return;
  client->reconnect_pending = 0;
  attempt = ++client->reconnect_attempt;
  rc = flowie_mqtt_client_connect_operation(client, &client->reconnect_connect->packet.connect,
                                             &response);
  if (rc == SALTS_OK) response_ptr = &response;
  flowie_mqtt_client_report_reconnect(client, attempt, rc, response_ptr);
  if (flowie_mqtt_client_is_stopping(client)) return;
  if (rc == SALTS_OK && response.reason_code == 0u) {
    flowie_mqtt_client_reconnect_cancel(client, 0);
    return;
  }
  if (rc == SALTS_OK && flowie_mqtt_client_reconnect_reason(response.reason_code)) {
    flowie_mqtt_client_reconnect_backoff(client);
    flowie_mqtt_client_reconnect_schedule(client);
    return;
  }
  if (rc == SALTS_OK && flowie_mqtt_client_refresh_reason(response.reason_code)) {
    rc = flowie_mqtt_client_refresh_reconnect(client, response.reason_code);
    if (rc == SALTS_OK) {
      flowie_mqtt_client_reconnect_backoff(client);
      flowie_mqtt_client_reconnect_schedule(client);
    } else {
      flowie_mqtt_client_reconnect_cancel(client, 1);
      flowie_mqtt_client_report_error(client, rc);
    }
    return;
  }
  if (rc != SALTS_OK) {
    flowie_mqtt_client_reconnect_after_failure(client, rc, 1);
    return;
  }
  client->reconnect_pending = 0;
}

static void flowie_mqtt_client_run_command(flowie_mqtt_client_t *client,
                                           flowie_mqtt_client_command_t *command) {
  flowie_mqtt_control_packet_view_t response = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  const flowie_mqtt_control_packet_view_t *response_ptr = NULL;
  int rc;
  client->disconnect_reason_valid = 0;
  switch (command->type) {
  case FLOWIE_MQTT_CLIENT_COMMAND_CONNECT:
    if (client->resilience_enabled) {
      rc = flowie_mqtt_client_reconnect_connect_replace(client, &command->packet.connect);
      if (rc == SALTS_OK) flowie_mqtt_client_reconnect_cancel(client, 0);
    } else {
      rc = SALTS_OK;
    }
    if (rc == SALTS_OK) {
      rc = flowie_mqtt_client_connect_operation(client, &command->packet.connect, &response);
      if (rc == SALTS_OK) response_ptr = &response;
    }
    break;
  case FLOWIE_MQTT_CLIENT_COMMAND_PUBLISH:
    rc = flowie_mqtt_client_publish_operation(client, &command->packet.publish, &response);
    if (rc == SALTS_OK && command->packet.publish.qos != 0u) response_ptr = &response;
    break;
  case FLOWIE_MQTT_CLIENT_COMMAND_SUBSCRIBE:
    rc = flowie_mqtt_client_subscribe_operation(client, &command->packet.subscribe, &response);
    if (rc == SALTS_OK) response_ptr = &response;
    break;
  case FLOWIE_MQTT_CLIENT_COMMAND_UNSUBSCRIBE:
    rc = flowie_mqtt_client_unsubscribe_operation(client, &command->packet.unsubscribe, &response);
    if (rc == SALTS_OK) response_ptr = &response;
    break;
  case FLOWIE_MQTT_CLIENT_COMMAND_PING:
    rc = flowie_mqtt_client_ping_operation(client);
    break;
  case FLOWIE_MQTT_CLIENT_COMMAND_AUTH:
    rc = flowie_mqtt_client_auth_operation(client, command->packet.control.properties, &response);
    if (rc == SALTS_OK) response_ptr = &response;
    break;
  case FLOWIE_MQTT_CLIENT_COMMAND_DISCONNECT:
    rc = flowie_mqtt_client_disconnect_operation(client, command->packet.control.reason_code,
                                                 command->packet.control.properties);
    if (rc == SALTS_EOF || rc == SALTS_ECONNRESET) rc = SALTS_OK;
    break;
  default:
    rc = SALTS_EINVAL;
    break;
  }
  if (rc != SALTS_OK && flowie_mqtt_client_is_stopping(client)) {
    rc = SALTS_ESHUTDOWN;
    response_ptr = NULL;
  }
  flowie_mqtt_client_complete(client, command, rc, response_ptr);
  if (command->type == FLOWIE_MQTT_CLIENT_COMMAND_CONNECT && client->resilience_enabled &&
      !flowie_mqtt_client_is_stopping(client)) {
    if (rc == SALTS_OK && response_ptr && response.reason_code == 0u) {
      flowie_mqtt_client_reconnect_cancel(client, 0);
    } else if (rc == SALTS_OK && response_ptr &&
               flowie_mqtt_client_reconnect_reason(response.reason_code)) {
      flowie_mqtt_client_reconnect_schedule(client);
    } else if (rc == SALTS_OK && response_ptr &&
               flowie_mqtt_client_refresh_reason(response.reason_code)) {
      int refresh_rc = flowie_mqtt_client_refresh_reconnect(client, response.reason_code);
      if (refresh_rc == SALTS_OK)
        flowie_mqtt_client_reconnect_schedule(client);
      else {
        flowie_mqtt_client_reconnect_cancel(client, 1);
        flowie_mqtt_client_report_error(client, refresh_rc);
      }
    } else if (rc != SALTS_OK) {
      flowie_mqtt_client_reconnect_after_failure(client, rc, 0);
    } else {
      flowie_mqtt_client_reconnect_cancel(client, 1);
    }
  } else if (command->type == FLOWIE_MQTT_CLIENT_COMMAND_DISCONNECT) {
    flowie_mqtt_client_reconnect_cancel(client, 1);
  } else if (rc != SALTS_OK && client->resilience_enabled &&
             !flowie_mqtt_client_is_stopping(client)) {
    flowie_mqtt_client_reconnect_after_failure(client, rc, 0);
  }
}

static void flowie_mqtt_client_worker_pump(flowie_mqtt_client_t *client) {
  for (;;) {
    flowie_mqtt_client_command_t *command = NULL;
    if (flowie_mqtt_client_command_pop(client, &command)) {
      flowie_mqtt_client_transport_close(client, 1);
      flowie_mqtt_client_reconnect_cancel(client, 1);
      flowie_mqtt_client_cancel_commands(client, SALTS_ESHUTDOWN);
      return;
    }
    if (command) {
      flowie_mqtt_client_run_command(client, command);
      flowie_mqtt_client_command_destroy(command);
      continue;
    }
    if (client->reconnect_pending) {
      uint64_t now = salts_monotonic_ms();
      uint64_t remaining_ms =
          client->reconnect_deadline_ms > now ? client->reconnect_deadline_ms - now : 0u;
      if (remaining_ms != 0u) {
        salts_mutex_lock(&client->command_mutex);
        if (!client->stopping && deque_empty(&client->commands))
          (void)salts_cond_timedwait(&client->command_changed, &client->command_mutex,
                                     remaining_ms > UINT64_MAX / UINT64_C(1000000)
                                         ? UINT64_MAX
                                         : remaining_ms * UINT64_C(1000000));
        salts_mutex_unlock(&client->command_mutex);
        if (flowie_mqtt_client_is_stopping(client) ||
            salts_monotonic_ms() < client->reconnect_deadline_ms)
          continue;
      }
      flowie_mqtt_client_run_reconnect(client);
      continue;
    }
    if (client->state == FLOWIE_MQTT_CLIENT_CONNECTED) {
      flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
      int handled = 0;
      int rc;
      client->disconnect_reason_valid = 0;
      rc = flowie_mqtt_client_receive_packet(client, &packet);
      if (rc == SALTS_EINTR) continue;
      if (rc == SALTS_OK) {
        rc = flowie_mqtt_client_handle_unsolicited(client, &packet, &handled);
        if (rc == SALTS_OK && !handled && packet.type != FLOWIE_MQTT_PACKET_PINGRESP)
          rc = SALTS_EPROTO;
      }
      if (rc != SALTS_OK) {
        flowie_mqtt_client_transport_close(client, 1);
        flowie_mqtt_client_report_error(client, rc);
        flowie_mqtt_client_reconnect_after_failure(client, rc, 0);
      }
      continue;
    }
    salts_mutex_lock(&client->command_mutex);
    while (!client->stopping && deque_empty(&client->commands) &&
           !client->reconnect_pending)
      salts_cond_wait(&client->command_changed, &client->command_mutex);
    salts_mutex_unlock(&client->command_mutex);
  }
}

static void flowie_mqtt_client_worker(void *arg) {
  flowie_mqtt_client_t *client = (flowie_mqtt_client_t *)arg;
  flowie_mqtt_client_current = client;
  flowie_mqtt_client_worker_pump(client);
  flowie_mqtt_client_current = NULL;
}

static flowie_mqtt_version_t *
flowie_mqtt_client_command_version(flowie_mqtt_client_command_t *command) {
  if (!command) return NULL;
  switch (command->type) {
  case FLOWIE_MQTT_CLIENT_COMMAND_CONNECT:
    return &command->packet.connect.version;
  case FLOWIE_MQTT_CLIENT_COMMAND_PUBLISH:
    return &command->packet.publish.version;
  case FLOWIE_MQTT_CLIENT_COMMAND_SUBSCRIBE:
    return &command->packet.subscribe.version;
  case FLOWIE_MQTT_CLIENT_COMMAND_UNSUBSCRIBE:
    return &command->packet.unsubscribe.version;
  default:
    return NULL;
  }
}

static int flowie_mqtt_client_command_version_resolve_locked(
    const flowie_mqtt_client_t *client, flowie_mqtt_client_command_t *command,
    int *versioned_out) {
  flowie_mqtt_version_t *version;
  if (!client || !command || !versioned_out) return SALTS_EINVAL;
  *versioned_out = 0;
  version = flowie_mqtt_client_command_version(command);
  if (!version) return SALTS_OK;
  *versioned_out = 1;
  return flowie_mqtt_client_version_resolve(client, version);
}

static int flowie_mqtt_client_submit(flowie_mqtt_client_t *client,
                                     flowie_mqtt_client_command_t *command) {
  size_t charge;
  size_t queue_size;
  int versioned = 0;
  int rc;
  if (!client || !command) return SALTS_EINVAL;
  if (command->owned_size > SIZE_MAX - sizeof(*command)) return SALTS_EMSGSIZE;
  charge = sizeof(*command) + command->owned_size;
  salts_mutex_lock(&client->command_mutex);
  queue_size = deque_size(&client->commands);
  if (client->stopping) {
    rc = SALTS_ESHUTDOWN;
  } else if ((rc = flowie_mqtt_client_command_version_resolve_locked(
                  client, command, &versioned)) != SALTS_OK) {
    /* The caller retains ownership when validation rejects admission. */
  } else if (queue_size >= client->command_queue_capacity) {
    rc = SALTS_ENOSPC;
  } else if (charge > client->command_queue_max_bytes - client->command_queue_bytes) {
    rc = SALTS_ENOSPC;
  } else {
    rc = flowie_stl_error(deque_push_back(&client->commands, &command));
    if (rc == SALTS_OK) {
      client->command_queue_bytes += charge;
      if (versioned) client->version_locked = 1;
      salts_cond_signal(&client->command_changed);
      if (client->network_initialized) (void)cnet_client_wake(&client->network);
    }
  }
  salts_mutex_unlock(&client->command_mutex);
  return rc;
}

static int flowie_mqtt_client_submit_many(flowie_mqtt_client_t *client,
                                          flowie_mqtt_client_command_t *const *commands,
                                          size_t command_count) {
  size_t charge = 0u;
  size_t inserted = 0u;
  size_t queue_size;
  int any_versioned = 0;
  int rc = SALTS_OK;
  if (!client || !commands || command_count == 0u) return SALTS_EINVAL;
  for (size_t i = 0u; i < command_count; ++i) {
    size_t command_charge;
    if (!commands[i] || commands[i]->owned_size > SIZE_MAX - sizeof(*commands[i]))
      return SALTS_EMSGSIZE;
    command_charge = sizeof(*commands[i]) + commands[i]->owned_size;
    if (command_charge > SIZE_MAX - charge) return SALTS_EMSGSIZE;
    charge += command_charge;
  }

  salts_mutex_lock(&client->command_mutex);
  queue_size = deque_size(&client->commands);
  if (client->stopping) {
    rc = SALTS_ESHUTDOWN;
  } else {
    for (size_t i = 0u; rc == SALTS_OK && i < command_count; ++i) {
      int versioned = 0;
      rc = flowie_mqtt_client_command_version_resolve_locked(client, commands[i], &versioned);
      if (versioned) any_versioned = 1;
    }
  }
  if (rc != SALTS_OK) {
    /* Reject the complete batch before any queue ownership transfer. */
  } else if (queue_size > client->command_queue_capacity ||
             command_count > client->command_queue_capacity - queue_size) {
    rc = SALTS_ENOSPC;
  } else if (client->command_queue_bytes > client->command_queue_max_bytes ||
             charge > client->command_queue_max_bytes - client->command_queue_bytes) {
    rc = SALTS_ENOSPC;
  } else {
    for (; inserted < command_count; ++inserted) {
      rc = flowie_stl_error(deque_push_back(&client->commands, &commands[inserted]));
      if (rc != SALTS_OK) break;
    }
    if (rc == SALTS_OK) {
      client->command_queue_bytes += charge;
      if (any_versioned) client->version_locked = 1;
      salts_cond_signal(&client->command_changed);
      if (client->network_initialized) (void)cnet_client_wake(&client->network);
      salts_mutex_unlock(&client->command_mutex);
      return SALTS_OK;
    }
    while (inserted != 0u) {
      flowie_mqtt_client_command_t *rolled_back = NULL;
      --inserted;
      (void)deque_pop_back(&client->commands, &rolled_back);
    }
  }
  salts_mutex_unlock(&client->command_mutex);
  return rc;
}

int flowie_mqtt_client_create_ex(const flowie_mqtt_client_config_t *config,
                                 const flowie_mqtt_client_resilience_config_t *resilience,
                                 flowie_mqtt_client_t **out) {
  const flowie_mqtt_client_tls_config_t *tls;
  flowie_mqtt_client_t *client;
  size_t max_packet_size;
  int rc;
  if (out) *out = NULL;
  if (!out) return SALTS_EINVAL;
  rc = flowie_mqtt_client_config_validate(config);
  if (rc != SALTS_OK) return rc;
  rc = flowie_mqtt_client_resilience_validate(resilience);
  if (rc != SALTS_OK) return rc;
  max_packet_size = config->max_packet_size ? config->max_packet_size
                                            : FLOWIE_MQTT_CLIENT_DEFAULT_MAX_PACKET_SIZE;
  client = (flowie_mqtt_client_t *)calloc(1, sizeof(*client));
  if (!client) return SALTS_ENOMEM;
  atomic_init(&client->public_connected, 0);
  client->selected_version = FLOWIE_MQTT_VERSION_5;
  client->transport = config->transport;
  client->port = config->port;
  client->timeout_ms =
      config->timeout_ms ? config->timeout_ms : FLOWIE_MQTT_CLIENT_DEFAULT_TIMEOUT_MS;
  client->max_packet_size = max_packet_size;
  client->outbound_max_packet_size = max_packet_size;
  client->max_inbound_qos2 = config->max_inbound_qos2;
  client->stream_recv_buffer_bytes = config->stream_recv_buffer_bytes;
  client->socket_recv_buffer_bytes = config->socket_recv_buffer_bytes;
  client->socket_send_buffer_bytes = config->socket_send_buffer_bytes;
  client->server_receive_maximum = UINT16_MAX;
  client->server_maximum_qos = 2u;
  client->server_retain_available = 1u;
  rc = flowie_mqtt_client_clone_topic_handlers(client, &config->topic_handlers);
  if (rc != SALTS_OK) goto fail;
  client->on_connect = config->on_connect;
  client->on_publish = config->on_publish;
  client->on_subscribe = config->on_subscribe;
  client->on_unsubscribe = config->on_unsubscribe;
  client->on_ping = config->on_ping;
  client->on_auth_challenge = config->on_auth_challenge;
  client->on_auth = config->on_auth;
  client->on_disconnect = config->on_disconnect;
  client->on_error = config->on_error;
  client->user_data = config->user_data;
  if (resilience) {
    client->resilience_enabled = 1;
    client->reconnect_initial_delay_ms =
        resilience->initial_delay_ms ? resilience->initial_delay_ms
                                     : FLOWIE_MQTT_CLIENT_DEFAULT_RECONNECT_INITIAL_DELAY_MS;
    client->reconnect_max_delay_ms =
        resilience->max_delay_ms ? resilience->max_delay_ms
                                 : FLOWIE_MQTT_CLIENT_DEFAULT_RECONNECT_MAX_DELAY_MS;
    client->reconnect_max_attempts = resilience->max_attempts;
    client->refresh_connect = resilience->refresh_connect;
    client->on_reconnect = resilience->on_reconnect;
  }
  client->command_queue_capacity = config->command_queue_capacity
                                       ? config->command_queue_capacity
                                       : FLOWIE_MQTT_CLIENT_DEFAULT_COMMAND_QUEUE_CAPACITY;
  client->command_queue_max_bytes = config->command_queue_max_bytes
                                        ? config->command_queue_max_bytes
                                        : FLOWIE_MQTT_CLIENT_DEFAULT_COMMAND_QUEUE_BYTES;
  client->next_packet_id = 1u;
  client->host = tstr_dup(config->host);
  client->path = tstr_dup(config->path ? config->path : "/mqtt");
  tls = flowie_mqtt_client_tls_config(config);
  if (tls) {
    client->tls_ca_file = tls->ca_file ? tstr_dup(tls->ca_file) : NULL;
    client->tls_cert_file = tls->cert_file ? tstr_dup(tls->cert_file) : NULL;
    client->tls_key_file = tls->key_file ? tstr_dup(tls->key_file) : NULL;
    client->tls_key_password = tls->key_password ? tstr_dup(tls->key_password) : NULL;
    client->tls_configured = tls->ca_file || tls->cert_file || tls->key_file || tls->key_password;
  }
  client->send_buffer = tstr_new_len(NULL, max_packet_size);
  if (!client->host || !client->path || !client->send_buffer ||
      (tls &&
       ((tls->ca_file && !client->tls_ca_file) || (tls->cert_file && !client->tls_cert_file) ||
        (tls->key_file && !client->tls_key_file) ||
        (tls->key_password && !client->tls_key_password)))) {
    rc = SALTS_ENOMEM;
    goto fail;
  }
  rc = salts_bytes_init(&client->framing, max_packet_size);
  if (rc != SALTS_OK) goto fail;
  client->framing_initialized = 1;
  rc = flowie_stl_error(hash_set_init_bytes(
      &client->inbound_qos2, sizeof(uint16_t), _Alignof(uint16_t),
      client->max_inbound_qos2, hash_bytes, hash_key_equal, NULL));
  if (rc != SALTS_OK) goto fail;
  client->qos2_initialized = 1;
  rc = flowie_stl_error(hash_set_reserve(&client->inbound_qos2, client->max_inbound_qos2));
  if (rc != SALTS_OK) goto fail;
  salts_mutex_init(&client->command_mutex);
  salts_cond_init(&client->command_changed);
  client->sync_initialized = 1;
  if (!client->command_mutex || !client->command_changed) {
    rc = SALTS_ENOMEM;
    goto fail;
  }
  rc = flowie_stl_error(deque_init_bytes(
      &client->commands, sizeof(flowie_mqtt_client_command_t *),
      _Alignof(flowie_mqtt_client_command_t *), client->command_queue_capacity));
  if (rc != SALTS_OK) goto fail;
  client->command_queue_initialized = 1;
  rc =
      flowie_stl_error(deque_reserve(&client->commands, client->command_queue_capacity));
  if (rc != SALTS_OK) goto fail;
  if (!flowie_mqtt_client_is_websocket(client)) {
    const cnet_client_config network_config = flowie_mqtt_client_network_config(client);
    const cnet_stream_socket_options socket_options =
        flowie_mqtt_client_socket_options(client);
    rc = cnet_client_init(&client->network, &network_config);
    if (rc != SALTS_OK) goto fail;
    client->network_initialized = 1;
    rc = cnet_client_set_stream_socket_options(&client->network, &socket_options);
    if (rc != SALTS_OK) goto fail;
  }
  if (client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WSS && client->tls_configured) {
    const cnet_tls_client_config tls_config = {.size = sizeof(tls_config),
                                               .ca_file = client->tls_ca_file,
                                               .cert_file = client->tls_cert_file,
                                               .key_file = client->tls_key_file,
                                               .key_password = client->tls_key_password,
                                               .server_name = client->host};
    rc = chttp_tls_profile_init(&client->websocket_tls, &tls_config);
    if (rc != SALTS_OK) goto fail;
    client->websocket_tls_initialized = 1;
  }
  rc = salts_thread_create(&client->worker, flowie_mqtt_client_worker, client);
  if (rc != SALTS_OK) goto fail;
  client->worker_started = 1;
  *out = client;
  return SALTS_OK;

fail:
  flowie_mqtt_client_destroy(client);
  return rc;
}

int flowie_mqtt_client_create(const flowie_mqtt_client_config_t *config,
                              flowie_mqtt_client_t **out) {
  return flowie_mqtt_client_create_ex(config, NULL, out);
}

void flowie_mqtt_client_destroy(flowie_mqtt_client_t *client) {
  flowie_mqtt_client_command_t *command = NULL;
  if (!client) return;
  if (client->worker_started) {
    if (flowie_mqtt_client_current == client) {
      salts_mutex_lock(&client->command_mutex);
      client->stopping = 1;
      salts_cond_signal(&client->command_changed);
      salts_mutex_unlock(&client->command_mutex);
      return;
    }
    salts_mutex_lock(&client->command_mutex);
    client->stopping = 1;
    salts_cond_signal(&client->command_changed);
    salts_mutex_unlock(&client->command_mutex);
    if (client->network_initialized) (void)cnet_client_wake(&client->network);
    if (salts_thread_join(&client->worker) != SALTS_OK) return;
    salts_thread_destroy(&client->worker);
    client->worker_started = 0;
  }
  flowie_mqtt_client_transport_close(client, 1);
  if (client->command_queue_initialized) {
    while (deque_pop_front(&client->commands, &command) == STL_OK)
      flowie_mqtt_client_command_destroy(command);
    deque_destroy(&client->commands);
  }
  flowie_mqtt_client_command_destroy(client->reconnect_connect);
  client->reconnect_connect = NULL;
  if (client->network_initialized) {
    (void)cnet_client_stop(&client->network, (uint32_t)client->timeout_ms);
    (void)cnet_client_destroy(&client->network);
    client->network_initialized = 0;
  }
  if (client->websocket_tls_initialized) {
    (void)chttp_tls_profile_destroy(&client->websocket_tls);
    client->websocket_tls_initialized = 0;
  }
  if (client->sync_initialized) {
    salts_cond_destroy(&client->command_changed);
    salts_mutex_destroy(&client->command_mutex);
  }
  if (client->qos2_initialized) hash_set_destroy(&client->inbound_qos2);
  if (client->framing_initialized) salts_bytes_destroy(&client->framing);
  tstr_freep(&client->send_buffer);
  tstr_freep(&client->path);
  tstr_freep(&client->host);
  tstr_freep(&client->tls_ca_file);
  tstr_freep(&client->tls_cert_file);
  tstr_freep(&client->tls_key_file);
  if (client->tls_key_password) {
    crypto_wipe(client->tls_key_password, tstr_len(client->tls_key_password));
    tstr_freep(&client->tls_key_password);
  }
  free(client->topic_handlers);
  free(client);
}

static int flowie_mqtt_client_uri(flowie_mqtt_client_t *client, const char *scheme,
                                  const char *path, char **out) {
  int bracket_host;
  size_t scheme_size;
  size_t host_size;
  size_t path_size;
  size_t capacity;
  int written;
  if (!client || !scheme || !out) return SALTS_EINVAL;
  bracket_host = strchr(client->host, ':') != NULL && client->host[0] != '[';
  scheme_size = strlen(scheme);
  host_size = tstr_len(client->host);
  path_size = path ? strlen(path) : 0u;
  *out = NULL;
  if (scheme_size > SIZE_MAX - host_size - path_size - 32u) return SALTS_EMSGSIZE;
  capacity = scheme_size + host_size + path_size + 32u;
  *out = (char *)malloc(capacity);
  if (!*out) return SALTS_ENOMEM;
  written = bracket_host
                ? snprintf(*out, capacity, "%s://[%s]:%d%s", scheme, client->host,
                           client->port, path ? path : "")
                : snprintf(*out, capacity, "%s://%s:%d%s", scheme, client->host,
                           client->port, path ? path : "");
  if (written < 0 || (size_t)written >= capacity) {
    free(*out);
    *out = NULL;
    return SALTS_EMSGSIZE;
  }
  return SALTS_OK;
}

static int flowie_mqtt_client_transport_connect(flowie_mqtt_client_t *client) {
  char *uri = NULL;
  int rc;
  if (!client) return SALTS_EINVAL;
  if (flowie_mqtt_client_is_websocket(client)) {
    const cnet_client_config network = flowie_mqtt_client_network_config(client);
    const chttp_websocket_client_config config = {
        .size = sizeof(config),
        .network = network,
        .max_frame_bytes = client->max_packet_size,
        .max_message_bytes = client->max_packet_size,
        .max_buffered_input_bytes = client->max_packet_size + CNET_WEBSOCKET_MAX_HEADER_BYTES,
        .max_handshake_header_bytes = FLOWIE_MQTT_CLIENT_HANDSHAKE_HEADER_BYTES,
        .event_capacity = FLOWIE_MQTT_CLIENT_IO_EVENT_CAPACITY,
        .socket_options = flowie_mqtt_client_socket_options(client)};
    chttp_websocket_connect_options options = {0};
    unsigned int http_status = 0u;
    rc = flowie_mqtt_client_uri(
        client, client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WSS ? "wss" : "ws",
        client->path, &uri);
    if (rc != SALTS_OK) return rc;
    rc = chttp_websocket_client_init(&client->websocket, &config);
    if (rc == SALTS_OK) client->websocket_initialized = 1;
    if (rc == SALTS_OK) {
      options.size = sizeof(options);
      options.uri = uri;
      options.tls = client->websocket_tls_initialized ? &client->websocket_tls : NULL;
      options.timeout_ms = (uint32_t)client->timeout_ms;
      options.protocol = CHTTP_HTTP_1_1;
      options.subprotocol = "mqtt";
      rc = chttp_websocket_client_connect(&client->websocket, &options, &http_status);
    }
    free(uri);
    return rc;
  }
  if (!client->network_initialized) return SALTS_EINVAL;
  {
    cnet_tls_client_config tls = {.size = sizeof(tls),
                                  .ca_file = client->tls_ca_file,
                                  .cert_file = client->tls_cert_file,
                                  .key_file = client->tls_key_file,
                                  .key_password = client->tls_key_password,
                                  .server_name = client->host};
    cnet_connect_options options = {0};
    const uint64_t deadline_ms = salts_monotonic_ms() + client->timeout_ms;
    rc = flowie_mqtt_client_uri(
        client, client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_TLS ? "tls" : "tcp", NULL,
        &uri);
    if (rc != SALTS_OK) return rc;
    client->network_connected = 0;
    client->network_terminal = 0;
    client->network_status = SALTS_OK;
    options.uri = uri;
    options.tls = client->tls_configured ? &tls : NULL;
    options.observer = (cnet_observer){.on_state = flowie_mqtt_client_network_state,
                                       .on_receive = flowie_mqtt_client_network_receive,
                                       .user = client,
                                       .on_send = flowie_mqtt_client_network_send};
    rc = cnet_connect(&client->network, &options, &client->network_connection);
    free(uri);
    if (rc != SALTS_OK) return rc;
    while (!client->network_connected && !client->network_terminal) {
      size_t events = 0u;
      if (flowie_mqtt_client_is_stopping(client)) return SALTS_ESHUTDOWN;
      if (salts_monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
      rc = cnet_client_poll(&client->network, flowie_mqtt_client_poll_slice(deadline_ms), &events);
      if (rc != SALTS_OK) return rc;
    }
    return client->network_connected ? SALTS_OK
                                     : (client->network_status != SALTS_OK
                                            ? client->network_status
                                            : SALTS_ECONNRESET);
  }
}

static int flowie_mqtt_client_connect_operation(flowie_mqtt_client_t *client,
                                                const flowie_mqtt_connect_packet_t *packet,
                                                flowie_mqtt_control_packet_view_t *connack) {
  size_t written = 0u;
  int rc;
  if (!client || !packet || !connack || !flowie_mqtt_client_ack_output_valid(connack) ||
      !flowie_mqtt_version_is_supported(packet->version))
    return SALTS_EINVAL;
  rc = flowie_mqtt_client_begin(client, 0);
  if (rc != SALTS_OK) return rc;
  if (client->state != FLOWIE_MQTT_CLIENT_DISCONNECTED) {
    rc = SALTS_EALREADY;
    goto done;
  }
  rc = flowie_mqtt_connect_packet_encode(packet, (uint8_t *)client->send_buffer,
                                         client->max_packet_size, &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) {
    rc = flowie_mqtt_client_parse_status(rc);
    goto done;
  }
  rc = flowie_mqtt_client_auth_method_select(client, packet->version == FLOWIE_MQTT_VERSION_5
                                                         ? packet->properties
                                                         : (flowie_mqtt_span_t){0});
  if (rc != SALTS_OK) goto done;
  salts_bytes_reset(&client->framing);
  hash_set_clear(&client->inbound_qos2);
  client->version = packet->version;
  rc = flowie_mqtt_client_transport_connect(client);
  if (rc != SALTS_OK) goto fail;
  client->state = FLOWIE_MQTT_CLIENT_TRANSPORT_CONNECTED;
  rc = flowie_mqtt_client_send(client, written);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_mqtt_client_wait_control(client, FLOWIE_MQTT_PACKET_CONNACK, 0u, connack);
  if (rc != SALTS_OK) goto fail;
  if (connack->reason_code != 0u) {
    flowie_mqtt_client_transport_close(client, 0);
    rc = SALTS_OK;
    goto done;
  }
  rc = flowie_mqtt_client_negotiate_connack(client, connack);
  if (rc != SALTS_OK) goto fail;
  client->state = FLOWIE_MQTT_CLIENT_CONNECTED;
  atomic_store_explicit(&client->public_connected, 1, memory_order_release);
  rc = SALTS_OK;
  goto done;

fail:
  flowie_mqtt_client_transport_close(client, 1);
done:
  flowie_mqtt_client_end(client);
  return rc;
}

static int flowie_mqtt_client_publish_operation(flowie_mqtt_client_t *client,
                                                const flowie_mqtt_publish_packet_t *packet,
                                                flowie_mqtt_control_packet_view_t *ack) {
  flowie_mqtt_publish_packet_t encoded;
  flowie_mqtt_control_packet_view_t received = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  size_t written = 0u;
  uint16_t packet_id = 0u;
  int rc;
  if (!client || !packet || !flowie_mqtt_client_ack_output_valid(ack) || packet->packet_id != 0u)
    return SALTS_EINVAL;
  rc = flowie_mqtt_client_begin(client, 1);
  if (rc != SALTS_OK) return rc;
  encoded = *packet;
  if (encoded.version != client->version) {
    rc = SALTS_EPROTO;
    goto done;
  }
  rc = flowie_mqtt_client_publish_capabilities_validate(client, &encoded);
  if (rc != SALTS_OK) goto done;
  if (encoded.qos != 0u) {
    packet_id = flowie_mqtt_client_packet_id(client);
    encoded.packet_id = packet_id;
  }
  rc = flowie_mqtt_publish_packet_encode(&encoded, (uint8_t *)client->send_buffer,
                                         client->outbound_max_packet_size, &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) {
    rc = flowie_mqtt_client_parse_status(rc);
    goto done;
  }
  rc = flowie_mqtt_client_send(client, written);
  if (rc != SALTS_OK) goto fail;
  if (encoded.qos == 0u) {
    if (ack) *ack = received;
    goto done;
  }
  rc = flowie_mqtt_client_wait_control(
      client, encoded.qos == 1u ? FLOWIE_MQTT_PACKET_PUBACK : FLOWIE_MQTT_PACKET_PUBREC, packet_id,
      &received);
  if (rc != SALTS_OK) goto fail;
  if (encoded.qos == 1u || received.reason_code >= 0x80u) {
    if (ack) *ack = received;
    goto done;
  }
  rc = flowie_mqtt_client_send_control(client, FLOWIE_MQTT_PACKET_PUBREL, packet_id, 0u);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_mqtt_client_wait_control(client, FLOWIE_MQTT_PACKET_PUBCOMP, packet_id, &received);
  if (rc != SALTS_OK) goto fail;
  if (ack) *ack = received;
  goto done;

fail:
  flowie_mqtt_client_transport_close(client, 1);
done:
  flowie_mqtt_client_end(client);
  return rc;
}

static int flowie_mqtt_client_subscribe_operation(flowie_mqtt_client_t *client,
                                                  const flowie_mqtt_subscribe_packet_t *packet,
                                                  flowie_mqtt_control_packet_view_t *suback) {
  flowie_mqtt_subscribe_packet_t encoded;
  flowie_mqtt_control_packet_view_t received = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  size_t written = 0u;
  uint16_t packet_id;
  int rc;
  if (!client || !packet || !suback || !flowie_mqtt_client_ack_output_valid(suback) ||
      packet->packet_id != 0u)
    return SALTS_EINVAL;
  rc = flowie_mqtt_client_begin(client, 1);
  if (rc != SALTS_OK) return rc;
  encoded = *packet;
  if (encoded.version != client->version) {
    rc = SALTS_EPROTO;
    goto done;
  }
  packet_id = flowie_mqtt_client_packet_id(client);
  encoded.packet_id = packet_id;
  rc = flowie_mqtt_subscribe_packet_encode(&encoded, (uint8_t *)client->send_buffer,
                                           client->outbound_max_packet_size, &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) {
    rc = flowie_mqtt_client_parse_status(rc);
    goto done;
  }
  rc = flowie_mqtt_client_send(client, written);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_mqtt_client_wait_control(client, FLOWIE_MQTT_PACKET_SUBACK, packet_id, &received);
  if (rc != SALTS_OK) goto fail;
  if (received.reason_codes.size != encoded.subscription_count) {
    rc = SALTS_EPROTO;
    goto fail;
  }
  *suback = received;
  goto done;

fail:
  flowie_mqtt_client_transport_close(client, 1);
done:
  flowie_mqtt_client_end(client);
  return rc;
}

static int flowie_mqtt_client_unsubscribe_operation(flowie_mqtt_client_t *client,
                                                    const flowie_mqtt_unsubscribe_packet_t *packet,
                                                    flowie_mqtt_control_packet_view_t *unsuback) {
  flowie_mqtt_unsubscribe_packet_t encoded;
  flowie_mqtt_control_packet_view_t received = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  size_t written = 0u;
  uint16_t packet_id;
  int rc;
  if (!client || !packet || !unsuback || !flowie_mqtt_client_ack_output_valid(unsuback) ||
      packet->packet_id != 0u)
    return SALTS_EINVAL;
  rc = flowie_mqtt_client_begin(client, 1);
  if (rc != SALTS_OK) return rc;
  encoded = *packet;
  if (encoded.version != client->version) {
    rc = SALTS_EPROTO;
    goto done;
  }
  packet_id = flowie_mqtt_client_packet_id(client);
  encoded.packet_id = packet_id;
  rc = flowie_mqtt_unsubscribe_packet_encode(&encoded, (uint8_t *)client->send_buffer,
                                             client->outbound_max_packet_size, &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) {
    rc = flowie_mqtt_client_parse_status(rc);
    goto done;
  }
  rc = flowie_mqtt_client_send(client, written);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_mqtt_client_wait_control(client, FLOWIE_MQTT_PACKET_UNSUBACK, packet_id, &received);
  if (rc != SALTS_OK) goto fail;
  if (client->version == FLOWIE_MQTT_VERSION_5 &&
      received.reason_codes.size != encoded.filter_count) {
    rc = SALTS_EPROTO;
    goto fail;
  }
  *unsuback = received;
  goto done;

fail:
  flowie_mqtt_client_transport_close(client, 1);
done:
  flowie_mqtt_client_end(client);
  return rc;
}

static int flowie_mqtt_client_ping_operation(flowie_mqtt_client_t *client) {
  size_t written = 0u;
  int rc = flowie_mqtt_client_begin(client, 1);
  if (rc != SALTS_OK) return rc;
  rc = flowie_mqtt_pingreq_encode(client->version, (uint8_t *)client->send_buffer,
                                  client->outbound_max_packet_size, &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) {
    rc = flowie_mqtt_client_parse_status(rc);
    goto done;
  }
  rc = flowie_mqtt_client_send(client, written);
  if (rc == SALTS_OK)
    rc = flowie_mqtt_client_wait_control(client, FLOWIE_MQTT_PACKET_PINGRESP, 0u, NULL);
  if (rc != SALTS_OK) flowie_mqtt_client_transport_close(client, 1);
done:
  flowie_mqtt_client_end(client);
  return rc;
}

static int flowie_mqtt_client_auth_operation(flowie_mqtt_client_t *client,
                                             flowie_mqtt_span_t properties,
                                             flowie_mqtt_control_packet_view_t *auth) {
  flowie_mqtt_property_block_view_t auth_properties = FLOWIE_MQTT_PROPERTY_BLOCK_VIEW_INIT;
  int rc;
  if (!client || !auth || !flowie_mqtt_client_ack_output_valid(auth)) return SALTS_EINVAL;
  rc = flowie_mqtt_client_begin(client, 1);
  if (rc != SALTS_OK) return rc;
  if (client->version != FLOWIE_MQTT_VERSION_5) {
    rc = SALTS_ENOTSUP;
    goto done;
  }
  auth_properties.values = properties;
  rc = flowie_mqtt_client_auth_method_matches(client, &auth_properties, 1);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_mqtt_client_send_auth(client, 0x19u, properties);
  if (rc != SALTS_OK) goto fail;
  for (;;) {
    flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
    int handled = 0;
    rc = flowie_mqtt_client_receive_packet(client, &packet);
    if (rc != SALTS_OK) goto fail;
    if (packet.type == FLOWIE_MQTT_PACKET_AUTH) {
      flowie_mqtt_control_packet_view_t response = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
      rc = flowie_mqtt_control_packet_parse(&packet, &response);
      if (rc != FLOWIE_MQTT_PARSE_OK) {
        rc = flowie_mqtt_client_parse_status(rc);
        goto fail;
      }
      if (response.reason_code == 0x18u) {
        rc = flowie_mqtt_client_handle_auth_challenge(client, &packet, NULL);
        if (rc != SALTS_OK) goto fail;
        continue;
      }
      if (response.reason_code != 0u) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      rc = flowie_mqtt_client_auth_method_matches(client, &response.properties, 0);
      if (rc != SALTS_OK) goto fail;
      *auth = response;
      rc = SALTS_OK;
      goto done;
    }
    rc = flowie_mqtt_client_handle_unsolicited(client, &packet, &handled);
    if (rc != SALTS_OK || !handled) {
      if (rc == SALTS_OK) rc = SALTS_EPROTO;
      goto fail;
    }
  }

fail:
  flowie_mqtt_client_transport_close(client, 1);
done:
  flowie_mqtt_client_end(client);
  return rc;
}

static int flowie_mqtt_client_disconnect_operation(flowie_mqtt_client_t *client,
                                                   uint8_t reason_code,
                                                   flowie_mqtt_span_t properties) {
  flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  size_t written = 0u;
  int rc = flowie_mqtt_client_begin(client, 1);
  if (rc != SALTS_OK) return rc;
  packet.version = client->version;
  packet.type = FLOWIE_MQTT_PACKET_DISCONNECT;
  packet.reason_code = reason_code;
  packet.properties = properties;
  rc = flowie_mqtt_control_packet_encode(&packet, (uint8_t *)client->send_buffer,
                                         client->outbound_max_packet_size, &written);
  if (rc == FLOWIE_MQTT_PARSE_OK) rc = flowie_mqtt_client_send(client, written);
  else rc = flowie_mqtt_client_parse_status(rc);
  flowie_mqtt_client_transport_close(client, 1);
  flowie_mqtt_client_end(client);
  return rc;
}

int flowie_mqtt_client_is_connected(const flowie_mqtt_client_t *client) {
  return client && atomic_load_explicit(&client->public_connected, memory_order_acquire);
}

int flowie_mqtt_client_server_disconnect_reason(const flowie_mqtt_client_t *client,
                                                 uint8_t *reason) {
  if (!client || !reason) return SALTS_EINVAL;
  if (!client->callback_active) return SALTS_EBUSY;
  if (!client->disconnect_reason_valid) return SALTS_ENOENT;
  *reason = client->disconnect_reason;
  return SALTS_OK;
}

int flowie_mqtt_client_set_version(flowie_mqtt_client_t *client,
                                   flowie_mqtt_version_t version) {
  int rc;
  if (!client || !flowie_mqtt_version_is_supported(version)) return SALTS_EINVAL;
  salts_mutex_lock(&client->command_mutex);
  if (client->stopping) rc = SALTS_ESHUTDOWN;
  else if (client->version_locked) rc = SALTS_EALREADY;
  else {
    client->selected_version = version;
    rc = SALTS_OK;
  }
  salts_mutex_unlock(&client->command_mutex);
  return rc;
}

static int flowie_mqtt_client_submit_owned(flowie_mqtt_client_t *client,
                                           flowie_mqtt_client_command_t *command, int clone_rc) {
  int rc = clone_rc;
  if (rc == SALTS_OK) rc = flowie_mqtt_client_submit(client, command);
  if (rc != SALTS_OK) flowie_mqtt_client_command_destroy(command);
  return rc;
}

int flowie_mqtt_client_connect(flowie_mqtt_client_t *client,
                               const flowie_mqtt_connect_packet_t *packet) {
  flowie_mqtt_client_command_t *command;
  if (!client || !packet) return SALTS_EINVAL;
  if (!client->on_connect) return SALTS_ENOTSUP;
  command = flowie_mqtt_client_command_new(FLOWIE_MQTT_CLIENT_COMMAND_CONNECT, client->on_connect,
                                           client->user_data);
  if (!command) return SALTS_ENOMEM;
  return flowie_mqtt_client_submit_owned(client, command,
                                         flowie_mqtt_client_clone_connect(command, packet));
}

int flowie_mqtt_client_publish(flowie_mqtt_client_t *client,
                               const flowie_mqtt_client_publish_topic_vec_t *topics) {
  vec_t commands = {0};
  int rc;
  if (!client || !topics || topics->size != sizeof(*topics) || !topics->data ||
      topics->count == 0u)
    return SALTS_EINVAL;
  if (!client->on_publish) return SALTS_ENOTSUP;
  if (topics->count > client->command_queue_capacity) return SALTS_ENOSPC;
  rc = flowie_stl_error(vec_init_bytes(
      &commands, sizeof(flowie_mqtt_client_command_t *),
      _Alignof(flowie_mqtt_client_command_t *), topics->count));
  if (rc != SALTS_OK) return rc;
  rc = flowie_stl_error(vec_reserve(&commands, topics->count));
  for (size_t i = 0u; rc == SALTS_OK && i < topics->count; ++i) {
    flowie_mqtt_client_command_t *command = flowie_mqtt_client_command_new(
        FLOWIE_MQTT_CLIENT_COMMAND_PUBLISH, client->on_publish, client->user_data);
    if (!command) {
      rc = SALTS_ENOMEM;
      break;
    }
    rc = flowie_mqtt_client_clone_publish_topic(command, topics->version, &topics->data[i]);
    if (rc == SALTS_OK) rc = flowie_stl_error(vec_push(&commands, &command));
    if (rc != SALTS_OK) flowie_mqtt_client_command_destroy(command);
  }
  if (rc == SALTS_OK)
    rc = flowie_mqtt_client_submit_many(client, vec_data(&commands),
                                        vec_size(&commands));
  if (rc != SALTS_OK) {
    for (size_t i = 0u; i < vec_size(&commands); ++i)
      flowie_mqtt_client_command_destroy(
          *(flowie_mqtt_client_command_t **)vec_at(&commands, i));
  }
  vec_destroy(&commands);
  return rc;
}

int flowie_mqtt_client_subscribe(flowie_mqtt_client_t *client,
                                 const flowie_mqtt_subscribe_packet_t *packet) {
  flowie_mqtt_client_command_t *command;
  if (!client || !packet) return SALTS_EINVAL;
  if (!client->on_subscribe) return SALTS_ENOTSUP;
  command = flowie_mqtt_client_command_new(FLOWIE_MQTT_CLIENT_COMMAND_SUBSCRIBE,
                                           client->on_subscribe, client->user_data);
  if (!command) return SALTS_ENOMEM;
  return flowie_mqtt_client_submit_owned(client, command,
                                         flowie_mqtt_client_clone_subscribe(command, packet));
}

int flowie_mqtt_client_unsubscribe(flowie_mqtt_client_t *client,
                                   const flowie_mqtt_unsubscribe_packet_t *packet) {
  flowie_mqtt_client_command_t *command;
  if (!client || !packet) return SALTS_EINVAL;
  if (!client->on_unsubscribe) return SALTS_ENOTSUP;
  command = flowie_mqtt_client_command_new(FLOWIE_MQTT_CLIENT_COMMAND_UNSUBSCRIBE,
                                           client->on_unsubscribe, client->user_data);
  if (!command) return SALTS_ENOMEM;
  return flowie_mqtt_client_submit_owned(client, command,
                                         flowie_mqtt_client_clone_unsubscribe(command, packet));
}

int flowie_mqtt_client_ping(flowie_mqtt_client_t *client) {
  flowie_mqtt_client_command_t *command;
  int rc;
  if (!client) return SALTS_EINVAL;
  if (!client->on_ping) return SALTS_ENOTSUP;
  command = flowie_mqtt_client_command_new(FLOWIE_MQTT_CLIENT_COMMAND_PING, client->on_ping,
                                           client->user_data);
  if (!command) return SALTS_ENOMEM;
  rc = flowie_mqtt_client_submit(client, command);
  if (rc != SALTS_OK) flowie_mqtt_client_command_destroy(command);
  return rc;
}

int flowie_mqtt_client_authenticate(flowie_mqtt_client_t *client, flowie_mqtt_span_t properties) {
  flowie_mqtt_client_command_t *command;
  uint8_t *cursor;
  int rc;
  if (!client || !flowie_mqtt_client_span_valid(properties)) return SALTS_EINVAL;
  if (!client->on_auth) return SALTS_ENOTSUP;
  command = flowie_mqtt_client_command_new(FLOWIE_MQTT_CLIENT_COMMAND_AUTH, client->on_auth,
                                           client->user_data);
  if (!command) return SALTS_ENOMEM;
  rc = flowie_mqtt_client_command_allocate(command, properties.size, &cursor);
  if (rc == SALTS_OK) {
    command->packet.control.reason_code = 0x19u;
    flowie_mqtt_client_copy_span(properties, &cursor, &command->packet.control.properties);
    rc = flowie_mqtt_client_submit(client, command);
  }
  if (rc != SALTS_OK) flowie_mqtt_client_command_destroy(command);
  return rc;
}

int flowie_mqtt_client_disconnect(flowie_mqtt_client_t *client, uint8_t reason_code,
                                  flowie_mqtt_span_t properties) {
  flowie_mqtt_client_command_t *command;
  uint8_t *cursor;
  int rc;
  if (!client || !flowie_mqtt_client_span_valid(properties)) return SALTS_EINVAL;
  if (!client->on_disconnect) return SALTS_ENOTSUP;
  command = flowie_mqtt_client_command_new(FLOWIE_MQTT_CLIENT_COMMAND_DISCONNECT,
                                           client->on_disconnect, client->user_data);
  if (!command) return SALTS_ENOMEM;
  rc = flowie_mqtt_client_command_allocate(command, properties.size, &cursor);
  if (rc == SALTS_OK) {
    command->packet.control.reason_code = reason_code;
    flowie_mqtt_client_copy_span(properties, &cursor, &command->packet.control.properties);
    rc = flowie_mqtt_client_submit(client, command);
  }
  if (rc != SALTS_OK) flowie_mqtt_client_command_destroy(command);
  return rc;
}
