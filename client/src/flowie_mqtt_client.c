#include "flowie_mqtt_client.h"

#include "flowie_stl_error_internal.h"

#include <cstl.h>
#include <cstl.h>
#include <cstl.h>
#include <cstl.h>

#include <http_client/http.h>
#include <cnet/cnet.h>
#include <cnet/manager.h>
#include <cnet/managed_dial.h>
#include <cnet/destination_policy.h>
#include <cnet/websocket.h>
#include "flowie_mqtt_protocol.h"
#include "monocypher.h"
#include "cmeta_bytes.h"
#include "cmeta_error.h"
#include "tstr.h"
#include "cmeta_thread.h"

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

typedef struct flowie_mqtt_owned_endpoint_s {
  tstr host;
  uint64_t endpoint_id;
  uint64_t inflight;
  uint32_t weight;
  uint16_t port;
  uint8_t eligible;
} flowie_mqtt_owned_endpoint_t;

struct flowie_mqtt_client_s {
  cnet_client network;
  cnet_manager network_manager;
  cnet_managed_dial network_dial;
  cnet_tls_client_config network_dial_tls;
  cnet_connection network_connection;
  chttp_websocket_client websocket;
  chttp_tls_profile websocket_tls;
  flowie_mqtt_client_transport_t transport;
  flowie_mqtt_client_state_t state;
  flowie_mqtt_version_t selected_version;
  flowie_mqtt_version_t version;
  tstr host;
  flowie_mqtt_owned_endpoint_t *destinations;
  size_t destination_count;
  size_t selected_destination_index;
  flowie_mqtt_client_destination_kind_t destination_kind;
  uint64_t destination_generation;
  uint64_t destination_expires_at_ms;
  uint64_t destination_sequence;
  uint64_t destination_explicit_id;
  uint64_t destination_key_hash;
  uint8_t destination_key_known;
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
  cmeta_bytes_t framing;
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
  int worker_ready;
  int worker_start_status;
  int destroy_in_progress;
  int worker_cleanup_result_ready;
  int worker_cleanup_complete;
  int worker_cleanup_retry;
  int worker_cleanup_status;
  uint32_t worker_cleanup_budget_ms;
  int network_closing;
  int stopping;
  int version_locked;
  size_t command_queue_capacity;
  size_t command_queue_max_bytes;
  size_t command_queue_bytes;
  deque_t commands;
  cmeta_mutex_t command_mutex;
  cmeta_cond_t command_changed;
  cmeta_thread_t worker;
  int network_initialized;
  int network_manager_initialized;
  int network_dial_initialized;
  int websocket_initialized;
  int websocket_tls_initialized;
  int network_connected;
  int network_terminal;
  int network_receive_ready;
  int network_send_ready;
  int network_status;
  int network_security_failure;
  atomic_int public_connected;
#if defined(FLOWIE_CLIENT_FAULT_TEST)
  /* Private to the separate fault-test DLL; absent from the shipped Client. */
  atomic_int test_close_full_enabled;
  atomic_uint test_close_full_hits;
  atomic_uint test_close_full_progress;
  atomic_uint test_close_full_wrong_owner;
  atomic_int test_native_stop_mode;
  atomic_uint test_native_stop_timeout_hits;
  atomic_int test_websocket_destroy_timeout;
  atomic_uint test_websocket_destroy_timeout_hits;
#endif
};

static SALTS_THREAD_LOCAL flowie_mqtt_client_t *flowie_mqtt_client_current;
static int flowie_mqtt_client_seal_managed_dial(flowie_mqtt_client_t *client);

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
  const uint64_t now = cmeta_monotonic_ms();
  const uint64_t remaining = deadline_ms > now ? deadline_ms - now : 0u;
  return (uint32_t)(remaining < FLOWIE_MQTT_CLIENT_IO_POLL_SLICE_MS
                        ? remaining
                        : FLOWIE_MQTT_CLIENT_IO_POLL_SLICE_MS);
}

static int flowie_mqtt_client_should_interrupt(flowie_mqtt_client_t *client) {
  int stopping;
  int queued;
  cmeta_mutex_lock(&client->command_mutex);
  stopping = client->stopping;
  queued = !deque_empty(&client->commands);
  cmeta_mutex_unlock(&client->command_mutex);
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

/* All calls are on the MQTT worker Owner, outside CNet callbacks. Manager
 * retires real native terminal records before the next physical admission. */
static int flowie_mqtt_client_manager_advance(flowie_mqtt_client_t *client) {
  size_t work = 0u;
  if (!client || !client->network_manager_initialized) return SALTS_OK;
  return cnet_manager_advance(&client->network_manager, 1u, &work);
}

static int flowie_mqtt_client_poll_managed(flowie_mqtt_client_t *client, uint32_t timeout_ms,
                                           size_t *events) {
  const int status = cnet_client_poll(&client->network, timeout_ms, events);
#if defined(FLOWIE_CLIENT_FAULT_TEST)
  /* Native progress during pre-cleanup transport_close is still real Owner
   * progress. The initial cleanup attempt may time out before reaching the
   * later final-destroy loop, but it must never free this Client. */
  if (atomic_load_explicit(&client->test_close_full_hits, memory_order_acquire) != 0u)
    atomic_fetch_add_explicit(&client->test_close_full_progress, 1u, memory_order_relaxed);
#endif
  return status == SALTS_OK ? flowie_mqtt_client_manager_advance(client) : status;
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

/* This isolated fault seam preserves a REAL CHttp WS/WSS backend on
 * forced destroy timeout. It is not compiled into production Flowie::Client.
 * The original Worker Owner performs the eventual real CHttp destruction. */
#if defined(FLOWIE_CLIENT_FAULT_TEST)
FLOWIE_MQTT_CLIENT_C_API int
flowie_mqtt_client_test_set_ws_destroy_timeout(flowie_mqtt_client_t *client, int enabled) {
  if (!client) return SALTS_EINVAL;
  atomic_store_explicit(&client->test_websocket_destroy_timeout,
                        enabled != 0, memory_order_release);
  return SALTS_OK;
}

FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_ws_destroy_timeout_hits(const flowie_mqtt_client_t *client) {
  return client ? atomic_load_explicit(&client->test_websocket_destroy_timeout_hits,
                                       memory_order_acquire) : 0u;
}
#endif

static int flowie_mqtt_client_destroy_websocket(flowie_mqtt_client_t *client,
                                                 uint32_t timeout_ms) {
#if defined(FLOWIE_CLIENT_FAULT_TEST)
  if (atomic_load_explicit(&client->test_websocket_destroy_timeout, memory_order_acquire)) {
    atomic_fetch_add_explicit(&client->test_websocket_destroy_timeout_hits,
                              1u, memory_order_relaxed);
    return SALTS_ETIMEDOUT;
  }
#endif
  return chttp_websocket_client_destroy(&client->websocket, timeout_ms);
}

static void flowie_mqtt_client_transport_close(flowie_mqtt_client_t *client, int reset_framing) {
  if (!client) return;
  flowie_mqtt_client_recv_release(client);
  if (client->websocket_initialized) {
    const int close_status = chttp_websocket_client_close(
        &client->websocket, 1000u, NULL, 0u, (uint32_t)client->timeout_ms);
    (void)close_status;
    (void)flowie_mqtt_client_destroy_websocket(client, (uint32_t)client->timeout_ms);
    /* Do not discard a live CHttp owner with pending native callbacks. */
    if (client->websocket.impl == NULL) client->websocket_initialized = 0;
  }
  if (client->network_initialized &&
      (client->network_connection.slot != 0u || client->network_dial_initialized)) {
    const uint64_t now = cmeta_monotonic_ms();
    const uint64_t deadline = client->timeout_ms > UINT64_MAX - now
                                  ? UINT64_MAX : now + client->timeout_ms;
    int close_status = SALTS_OK;
    if (client->network_dial_initialized)
      close_status = flowie_mqtt_client_seal_managed_dial(client);
    else if (client->network_connection.slot != 0u && !client->network_terminal)
      close_status = cnet_close(&client->network, client->network_connection);
    while (cmeta_monotonic_ms() < deadline &&
           client->network_connection.slot != 0u &&
           !client->network_terminal) {
      size_t events = 0u;
      const uint32_t slice = flowie_mqtt_client_poll_slice(deadline);
      if (client->network_dial_initialized && close_status != SALTS_OK)
        close_status = flowie_mqtt_client_seal_managed_dial(client);
      if (flowie_mqtt_client_poll_managed(client, slice, &events) != SALTS_OK)
        break;
    }
    (void)flowie_mqtt_client_manager_advance(client);
    if (client->network_dial_initialized) {
      /* Destroy requires Manager's *real* recycle, including a rejected
       * synchronous dial that never had an active CNet connection. */
      const int status = cnet_managed_dial_destroy(&client->network_dial);
      if (status == SALTS_OK) client->network_dial_initialized = 0;
    }
    if (!client->network_dial_initialized)
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
  if (reset_framing && client->framing_initialized) cmeta_bytes_reset(&client->framing);
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
  now = cmeta_monotonic_ms();
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
    mem_buffer_t *buffer = mem_get_buffer(mem_global(), written);
    int rc;
    if (buffer == NULL) return SALTS_ENOMEM;
    /* A timeout may return while CNet still owns the in-flight payload. */
    memcpy(mem_buffer_data(buffer), client->send_buffer, written);
    mem_set_used(buffer, written);
    rc = cnet_send_buffer(&client->network, client->network_connection, buffer);
    mem_buffer_release(buffer);
    if (rc != SALTS_OK) return rc;
  }
  deadline_ms = cmeta_monotonic_ms() + client->timeout_ms;
  while (!client->network_send_ready && !client->network_terminal) {
    size_t events = 0u;
    int rc = flowie_mqtt_client_should_interrupt(client);
    if (rc == SALTS_ESHUTDOWN) return rc;
    if (cmeta_monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
    rc = flowie_mqtt_client_poll_managed(client, flowie_mqtt_client_poll_slice(deadline_ms), &events);
    if (rc != SALTS_OK) return rc;
  }
  return client->network_send_ready ? client->network_status
                                    : (client->network_status != SALTS_OK
                                           ? client->network_status
                                           : SALTS_ECONNRESET);
}

static int flowie_mqtt_client_transport_receive(flowie_mqtt_client_t *client) {
  const uint64_t deadline_ms = cmeta_monotonic_ms() + client->timeout_ms;
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
      if (cmeta_monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
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
    if (cmeta_monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
    rc = flowie_mqtt_client_poll_managed(client, flowie_mqtt_client_poll_slice(deadline_ms), &events);
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
    int rc = cmeta_bytes_consume(&client->framing, client->pending_packet_size);
    if (rc != SALTS_OK) return rc;
    client->pending_packet_size = 0u;
  }
  options.version = client->version;
  options.max_packet_size = client->max_packet_size;
  for (;;) {
    cmeta_bytes_view_t bytes;
    flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
    size_t consumed = 0u;
    int rc = cmeta_bytes_view(&client->framing, &bytes);
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
      size_t available = cmeta_bytes_available(&client->framing);
      size_t chunk = remaining < available ? remaining : available;
      if (chunk == 0u) return SALTS_EMSGSIZE;
      rc = cmeta_bytes_append(&client->framing, client->recv_data + client->recv_offset,
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
  /* Never schedule even the legacy MQTT CONNECT retry on a TLS trust/
   * handshake security failure. A new credential/trust configuration requires
   * an explicit new Client, not opportunistic plaintext or peer failover. */
  if (client->network_security_failure) {
    flowie_mqtt_client_reconnect_cancel(client, 1);
    return;
  }
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
  cmeta_mutex_lock(&client->command_mutex);
  stopping = client->stopping;
  if (!stopping) {
    if (deque_pop_front(&client->commands, out) != STL_OK) {
      *out = NULL;
    } else {
      client->command_queue_bytes -= sizeof(**out) + (*out)->owned_size;
    }
  }
  cmeta_mutex_unlock(&client->command_mutex);
  return stopping;
}

static void flowie_mqtt_client_cancel_commands(flowie_mqtt_client_t *client, int status) {
  for (;;) {
    flowie_mqtt_client_command_t *command = NULL;
    cmeta_mutex_lock(&client->command_mutex);
    (void)deque_pop_front(&client->commands, &command);
    if (command) client->command_queue_bytes -= sizeof(*command) + command->owned_size;
    cmeta_mutex_unlock(&client->command_mutex);
    if (!command) return;
    flowie_mqtt_client_complete(client, command, status, NULL);
    flowie_mqtt_client_command_destroy(command);
  }
}

static int flowie_mqtt_client_is_stopping(flowie_mqtt_client_t *client) {
  int stopping;
  cmeta_mutex_lock(&client->command_mutex);
  stopping = client->stopping;
  cmeta_mutex_unlock(&client->command_mutex);
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
      uint64_t now = cmeta_monotonic_ms();
      uint64_t remaining_ms =
          client->reconnect_deadline_ms > now ? client->reconnect_deadline_ms - now : 0u;
      if (remaining_ms != 0u) {
        cmeta_mutex_lock(&client->command_mutex);
        if (!client->stopping && deque_empty(&client->commands))
          (void)cmeta_cond_timedwait(&client->command_changed, &client->command_mutex,
                                     remaining_ms > UINT64_MAX / UINT64_C(1000000)
                                         ? UINT64_MAX
                                         : remaining_ms * UINT64_C(1000000));
        cmeta_mutex_unlock(&client->command_mutex);
        if (flowie_mqtt_client_is_stopping(client) ||
            cmeta_monotonic_ms() < client->reconnect_deadline_ms)
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
    cmeta_mutex_lock(&client->command_mutex);
    while (!client->stopping && deque_empty(&client->commands) &&
           !client->reconnect_pending)
      cmeta_cond_wait(&client->command_changed, &client->command_mutex);
    cmeta_mutex_unlock(&client->command_mutex);
  }
}

/* NativeIO backends (notably io_uring SINGLE_ISSUER) must be constructed on
 * their final network Owner. The worker exclusively owns CNet create/poll/stop
 * and destruction; callers only perform thread-safe command/wake admission. */
static int flowie_mqtt_client_worker_network_init(flowie_mqtt_client_t *client) {
  cnet_client_config network_config;
  cnet_stream_socket_options socket_options;
  int rc;
  if (flowie_mqtt_client_is_websocket(client)) return SALTS_OK;
  network_config = flowie_mqtt_client_network_config(client);
  socket_options = flowie_mqtt_client_socket_options(client);
  rc = cnet_client_init(&client->network, &network_config);
  if (rc != SALTS_OK) return rc;
  client->network_initialized = 1;
  rc = cnet_client_set_stream_socket_options(&client->network, &socket_options);
  if (rc != SALTS_OK) return rc;
  {
    const cnet_manager_config config = {
        .size = sizeof(config), .version = CNET_MANAGER_VERSION,
        .client = &client->network, .record_capacity = 1u, .connection_capacity = 1u};
    rc = cnet_manager_init(&client->network_manager, &config);
    if (rc == SALTS_OK) client->network_manager_initialized = 1;
  }
  return rc;
}

/* This seam exists only in the separate fault-test DLL, never in the installed
 * SDK. Managed Dial seal() (not Manager request_close()) can report ENOBUFS
 * when a CNet CLOSE command cannot be admitted. Never forge a terminal. */
#if defined(FLOWIE_CLIENT_FAULT_TEST)
FLOWIE_MQTT_CLIENT_C_API int
flowie_mqtt_client_test_force_close_full(flowie_mqtt_client_t *client, int enabled) {
  if (!client) return SALTS_EINVAL;
  atomic_store_explicit(&client->test_close_full_enabled, enabled != 0, memory_order_release);
  return SALTS_OK;
}

FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_close_full_hits(const flowie_mqtt_client_t *client) {
  return client ? atomic_load_explicit(&client->test_close_full_hits, memory_order_acquire) : 0u;
}

FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_close_full_progress(const flowie_mqtt_client_t *client) {
  return client ? atomic_load_explicit(&client->test_close_full_progress, memory_order_acquire)
                : 0u;
}

FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_close_full_wrong_owner(const flowie_mqtt_client_t *client) {
  return client ? atomic_load_explicit(&client->test_close_full_wrong_owner,
                                       memory_order_acquire) : 0u;
}
#endif

static int flowie_mqtt_client_seal_managed_dial(flowie_mqtt_client_t *client) {
#if defined(FLOWIE_CLIENT_FAULT_TEST)
  if (atomic_load_explicit(&client->test_close_full_enabled, memory_order_acquire)) {
    if (flowie_mqtt_client_current != client)
      atomic_fetch_add_explicit(&client->test_close_full_wrong_owner, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&client->test_close_full_hits, 1u, memory_order_relaxed);
    return SALTS_ENOBUFS;
  }
#endif
  return cnet_managed_dial_seal(&client->network_dial);
}

/* Private fault-test modes: 1 = observed progress error after a real,
 * completed native stop; 2 = synthetic timeout before native stop. Neither
 * mode changes the installed SDK or fabricates a native terminal. */
#if defined(FLOWIE_CLIENT_FAULT_TEST)
/* Static-storage atomics are zero-initialized by C; MSVC C11 rejects ATOMIC_VAR_INIT here. */
static atomic_uint flowie_mqtt_client_test_native_stop_error_count;

FLOWIE_MQTT_CLIENT_C_API int
flowie_mqtt_client_test_set_native_stop_mode(flowie_mqtt_client_t *client, int mode) {
  if (!client || mode < 0 || mode > 2) return SALTS_EINVAL;
  atomic_store_explicit(&client->test_native_stop_mode, mode, memory_order_release);
  return SALTS_OK;
}

FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_native_stop_timeout_hits(const flowie_mqtt_client_t *client) {
  return client ? atomic_load_explicit(&client->test_native_stop_timeout_hits,
                                        memory_order_acquire) : 0u;
}

FLOWIE_MQTT_CLIENT_C_API unsigned int
flowie_mqtt_client_test_native_stop_reported_errors(void) {
  return atomic_load_explicit(&flowie_mqtt_client_test_native_stop_error_count,
                              memory_order_acquire);
}
#endif

static int flowie_mqtt_client_stop_native(flowie_mqtt_client_t *client, uint32_t timeout_ms) {
#if defined(FLOWIE_CLIENT_FAULT_TEST)
  const int mode = atomic_load_explicit(&client->test_native_stop_mode, memory_order_acquire);
  if (flowie_mqtt_client_current != client) return SALTS_EPERM;
  if (mode == 2) {
    atomic_fetch_add_explicit(&client->test_native_stop_timeout_hits, 1u,
                              memory_order_relaxed);
    cmeta_sleep_ms(1u); /* Avoid a zero-progress synthetic busy spin. */
    return SALTS_ETIMEDOUT; /* Real CNet backend remains live. */
  }
#endif
  const int status = cnet_client_stop(&client->network, timeout_ms);
#if defined(FLOWIE_CLIENT_FAULT_TEST)
  if (mode == 1 && (status == SALTS_OK || status == SALTS_EALREADY)) {
    /* Native really stopped, but the observed return emulates the CNet 2.3
     * callback_error latch. Only cnet_client_destroy() authorizes release. */
    atomic_fetch_add_explicit(&flowie_mqtt_client_test_native_stop_error_count, 1u,
                              memory_order_relaxed);
    return SALTS_EIO;
  }
#endif
  return status;
}

/* Strictly Owner-local and retryable. Never let the Client allocation die while
 * any managed connection, callback or native backend still borrows it. */
static int flowie_mqtt_client_worker_network_destroy(flowie_mqtt_client_t *client,
                                                      uint32_t budget_ms) {
  const uint64_t start = cmeta_monotonic_ms();
  const uint64_t deadline = budget_ms > UINT64_MAX - start
                                ? UINT64_MAX : start + budget_ms;
  int status;
  if (client->websocket.impl != NULL) {
    const int ws_status = flowie_mqtt_client_destroy_websocket(client, budget_ms);
    if (client->websocket.impl != NULL)
      return ws_status == SALTS_OK ? SALTS_EBUSY : ws_status;
    client->websocket_initialized = 0;
  }
  if (client->network.impl == NULL)
    return client->network_manager.impl == NULL && client->network_dial.impl == NULL
               ? SALTS_OK : SALTS_EBUSY;
  if (client->network_dial.impl != NULL) {
    status = flowie_mqtt_client_seal_managed_dial(client);
    if (status != SALTS_OK && status != SALTS_EALREADY &&
        status != SALTS_ENOBUFS && status != SALTS_EBUSY)
      return status;
  }
  if (client->network_manager.impl != NULL) {
    /* Manager request_close seals state; actual CNet CLOSE admission occurs
     * in Managed Dial seal / Manager advance. Both can report ENOBUFS. */
    status = cnet_manager_request_close(&client->network_manager);
    if (status != SALTS_OK) return status;
    for (;;) {
      cnet_manager_snapshot snapshot = {0};
      size_t work = 0u, events = 0u;
      int seal_pending = 0;
      status = cnet_manager_advance(&client->network_manager, 1u, &work);
      if (status != SALTS_OK && status != SALTS_ENOBUFS && status != SALTS_EBUSY)
        return status;
#if defined(FLOWIE_CLIENT_FAULT_TEST)
      if (atomic_load_explicit(&client->test_close_full_hits, memory_order_acquire) != 0u)
        atomic_fetch_add_explicit(&client->test_close_full_progress, 1u, memory_order_relaxed);
#endif
      status = cnet_manager_get_snapshot(&client->network_manager, &snapshot);
      if (status != SALTS_OK) return status;
      /* A drained Manager is not proof that an earlier FULL Dial seal
       * admitted its close. Retry that independent obligation before stop. */
      if (client->network_dial.impl != NULL) {
        const int seal_status = flowie_mqtt_client_seal_managed_dial(client);
        if (seal_status == SALTS_ENOBUFS || seal_status == SALTS_EBUSY) {
          seal_pending = 1;
        } else if (seal_status != SALTS_OK && seal_status != SALTS_EALREADY) {
          return seal_status;
        }
      }
      if (snapshot.drained && !seal_pending) break;
      if (budget_ms == 0u) return SALTS_EBUSY;
      const uint64_t now = cmeta_monotonic_ms();
      if (now >= deadline) return SALTS_ETIMEDOUT;
      const uint32_t slice = (uint32_t)(deadline - now < FLOWIE_MQTT_CLIENT_IO_POLL_SLICE_MS
                                          ? deadline - now : FLOWIE_MQTT_CLIENT_IO_POLL_SLICE_MS);
      status = cnet_client_poll(&client->network, slice, &events);
      if (status != SALTS_OK) return status;
    }
  }
  /* The native stop status can carry an earlier callback failure even after
   * actual backend shutdown; only the successful destruction of the real
   * resources permits the outer Client allocation to be freed. */
  const uint64_t now = cmeta_monotonic_ms();
  const uint32_t remaining = now >= deadline ? 0u
                              : deadline - now > UINT32_MAX ? UINT32_MAX
                              : (uint32_t)(deadline - now);
  status = flowie_mqtt_client_stop_native(client, remaining);
  /* Salts 2.3 explicitly permits an earlier callback/progress error to be
   * returned even after native quiescence. The real cnet_client_destroy()
   * below—not a whitelist of historical status codes—is authoritative.
   * Only known unfinished stop states are deferred to a later same-Owner
   * attempt, without freeing anything borrowed by the native backend. */
  if (status == SALTS_ETIMEDOUT || status == SALTS_EBUSY ||
      status == SALTS_ENOTSUP) return status;
  if (client->network_dial.impl != NULL) {
    status = cnet_managed_dial_destroy(&client->network_dial);
    if (status != SALTS_OK) return status;
    client->network_dial_initialized = 0;
  }
  if (client->network_manager.impl != NULL) {
    status = cnet_manager_destroy(&client->network_manager);
    if (status != SALTS_OK) return status;
    client->network_manager_initialized = 0;
  }
  status = cnet_client_destroy(&client->network);
  if (client->network.impl != NULL) return status == SALTS_OK ? SALTS_EBUSY : status;
  client->network_initialized = 0;
  return SALTS_OK;
}

static void flowie_mqtt_client_worker(void *arg) {
  flowie_mqtt_client_t *client = (flowie_mqtt_client_t *)arg;
  int status;
  flowie_mqtt_client_current = client;
  status = flowie_mqtt_client_worker_network_init(client);
  cmeta_mutex_lock(&client->command_mutex);
  client->worker_start_status = status;
  client->worker_ready = 1;
  cmeta_cond_broadcast(&client->command_changed);
  cmeta_mutex_unlock(&client->command_mutex);
  if (status == SALTS_OK) flowie_mqtt_client_worker_pump(client);
  cmeta_mutex_lock(&client->command_mutex);
  client->network_closing = 1; /* Fence foreign native wakes. */
  cmeta_mutex_unlock(&client->command_mutex);
  for (;;) {
    uint32_t budget;
    cmeta_mutex_lock(&client->command_mutex);
    budget = client->worker_cleanup_budget_ms;
    cmeta_mutex_unlock(&client->command_mutex);
    status = flowie_mqtt_client_worker_network_destroy(client, budget);
    cmeta_mutex_lock(&client->command_mutex);
    client->worker_cleanup_status = status;
    client->worker_cleanup_result_ready = 1;
    if (status == SALTS_OK) client->worker_cleanup_complete = 1;
    cmeta_cond_broadcast(&client->command_changed);
    if (client->worker_cleanup_complete) {
      cmeta_mutex_unlock(&client->command_mutex);
      break;
    }
    /* Report incomplete drain while keeping this exact native Owner alive.
     * Another try_destroy may request a new bounded attempt. */
    while (!client->worker_cleanup_retry)
      cmeta_cond_wait(&client->command_changed, &client->command_mutex);
    client->worker_cleanup_retry = 0;
    client->worker_cleanup_result_ready = 0;
    cmeta_mutex_unlock(&client->command_mutex);
  }
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
  cmeta_mutex_lock(&client->command_mutex);
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
      cmeta_cond_signal(&client->command_changed);
      if (client->network_initialized) (void)cnet_client_wake(&client->network);
    }
  }
  cmeta_mutex_unlock(&client->command_mutex);
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

  cmeta_mutex_lock(&client->command_mutex);
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
      cmeta_cond_signal(&client->command_changed);
      if (client->network_initialized) (void)cnet_client_wake(&client->network);
      cmeta_mutex_unlock(&client->command_mutex);
      return SALTS_OK;
    }
    while (inserted != 0u) {
      flowie_mqtt_client_command_t *rolled_back = NULL;
      --inserted;
      (void)deque_pop_back(&client->commands, &rolled_back);
    }
  }
  cmeta_mutex_unlock(&client->command_mutex);
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
#if defined(FLOWIE_CLIENT_FAULT_TEST)
  atomic_init(&client->test_close_full_enabled, 0);
  atomic_init(&client->test_close_full_hits, 0u);
  atomic_init(&client->test_close_full_progress, 0u);
  atomic_init(&client->test_close_full_wrong_owner, 0u);
  atomic_init(&client->test_native_stop_mode, 0);
  atomic_init(&client->test_native_stop_timeout_hits, 0u);
  atomic_init(&client->test_websocket_destroy_timeout, 0);
  atomic_init(&client->test_websocket_destroy_timeout_hits, 0u);
#endif
  client->selected_version = FLOWIE_MQTT_VERSION_5;
  client->selected_destination_index = SIZE_MAX;
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
  rc = cmeta_bytes_init(&client->framing, max_packet_size);
  if (rc != SALTS_OK) goto fail;
  client->framing_initialized = 1;
  rc = flowie_stl_error(hash_set_init_bytes(
      &client->inbound_qos2, sizeof(uint16_t), _Alignof(uint16_t),
      client->max_inbound_qos2, hash_bytes, hash_key_equal, NULL));
  if (rc != SALTS_OK) goto fail;
  client->qos2_initialized = 1;
  rc = flowie_stl_error(hash_set_reserve(&client->inbound_qos2, client->max_inbound_qos2));
  if (rc != SALTS_OK) goto fail;
  cmeta_mutex_init(&client->command_mutex);
  cmeta_cond_init(&client->command_changed);
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
  rc = cmeta_thread_create(&client->worker, flowie_mqtt_client_worker, client);
  if (rc != SALTS_OK) goto fail;
  client->worker_started = 1;
  cmeta_mutex_lock(&client->command_mutex);
  while (!client->worker_ready)
    cmeta_cond_wait(&client->command_changed, &client->command_mutex);
  rc = client->worker_start_status;
  cmeta_mutex_unlock(&client->command_mutex);
  if (rc != SALTS_OK) goto fail;
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

static void flowie_mqtt_client_finalize(flowie_mqtt_client_t *client) {
  flowie_mqtt_client_command_t *command = NULL;
  /* All borrowed native contexts have completed before freeing user data. */
  flowie_mqtt_client_transport_close(client, 1);
  if (client->command_queue_initialized) {
    while (deque_pop_front(&client->commands, &command) == STL_OK)
      flowie_mqtt_client_command_destroy(command);
    deque_destroy(&client->commands);
  }
  flowie_mqtt_client_command_destroy(client->reconnect_connect);
  client->reconnect_connect = NULL;
  if (client->websocket_tls_initialized) {
    (void)chttp_tls_profile_destroy(&client->websocket_tls);
    client->websocket_tls_initialized = 0;
  }
  if (client->sync_initialized) {
    cmeta_cond_destroy(&client->command_changed);
    cmeta_mutex_destroy(&client->command_mutex);
  }
  if (client->qos2_initialized) hash_set_destroy(&client->inbound_qos2);
  if (client->framing_initialized) cmeta_bytes_destroy(&client->framing);
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
  for (size_t index = 0u; index < client->destination_count; ++index)
    tstr_freep(&client->destinations[index].host);
  free(client->destinations);
  free(client->topic_handlers);
  free(client);
}

int flowie_mqtt_client_try_destroy(flowie_mqtt_client_t *client, uint32_t timeout_ms) {
  int status;
  if (client == NULL) return SALTS_EINVAL;
  /* A Client callback is not allowed to wait for its own worker. Set the
   * stop intent, but retain ownership for an explicit external retry. */
  if (flowie_mqtt_client_current == client) {
    if (client->sync_initialized) {
      cmeta_mutex_lock(&client->command_mutex);
      client->stopping = 1;
      cmeta_cond_broadcast(&client->command_changed);
      cmeta_mutex_unlock(&client->command_mutex);
    }
    return SALTS_EBUSY;
  }
  if (client->worker_started) {
    const uint64_t start = cmeta_monotonic_ms();
    const uint64_t deadline = timeout_ms > UINT64_MAX - start
                                  ? UINT64_MAX : start + timeout_ms;
    cmeta_mutex_lock(&client->command_mutex);
    if (client->destroy_in_progress) {
      cmeta_mutex_unlock(&client->command_mutex);
      return SALTS_EBUSY;
    }
    client->destroy_in_progress = 1;
    client->stopping = 1;
    client->worker_cleanup_budget_ms = timeout_ms;
    if (client->worker_cleanup_result_ready && !client->worker_cleanup_complete) {
      client->worker_cleanup_result_ready = 0;
      client->worker_cleanup_retry = 1;
    }
    cmeta_cond_broadcast(&client->command_changed);
    if (!client->network_closing && client->network_initialized)
      (void)cnet_client_wake(&client->network);
    for (;;) {
      const uint64_t now = cmeta_monotonic_ms();
      if (client->worker_cleanup_result_ready) {
        const int rc = client->worker_cleanup_status;
        if (rc == SALTS_OK) break;
        if (timeout_ms == 0u || now >= deadline ||
            (rc != SALTS_EBUSY && rc != SALTS_ETIMEDOUT &&
             rc != SALTS_ENOBUFS)) break;
        /* One bounded try_destroy call may retry a FULL/unfinished native
         * drain, but only on the very same Owner and within its deadline. */
        client->worker_cleanup_result_ready = 0;
        client->worker_cleanup_retry = 1;
        client->worker_cleanup_budget_ms =
            deadline - now > UINT32_MAX ? UINT32_MAX : (uint32_t)(deadline - now);
        cmeta_cond_broadcast(&client->command_changed);
      }
      if (timeout_ms == 0u || now >= deadline) break;
      const uint64_t remaining_ms = deadline - now;
      const uint64_t ns = remaining_ms > UINT64_MAX / 1000000u
                              ? UINT64_MAX : remaining_ms * 1000000u;
      (void)cmeta_cond_timedwait(&client->command_changed, &client->command_mutex, ns);
    }
    status = client->worker_cleanup_result_ready ? client->worker_cleanup_status
                                                 : timeout_ms == 0u ? SALTS_EBUSY
                                                                    : SALTS_ETIMEDOUT;
    if (status != SALTS_OK || !client->worker_cleanup_complete) {
      client->destroy_in_progress = 0;
      cmeta_mutex_unlock(&client->command_mutex);
      return status == SALTS_OK ? SALTS_EBUSY : status;
    }
    cmeta_mutex_unlock(&client->command_mutex);
    status = cmeta_thread_join(&client->worker);
    if (status != SALTS_OK) {
      cmeta_mutex_lock(&client->command_mutex);
      client->destroy_in_progress = 0;
      cmeta_mutex_unlock(&client->command_mutex);
      return status;
    }
    cmeta_thread_destroy(&client->worker);
    client->worker_started = 0;
  }
  /* Fail closed: retaining a Client allocation is safer than releasing
   * storage still borrowed by a CNet/Dial/Manager observer. */
  if (client->network.impl != NULL || client->network_manager.impl != NULL ||
      client->network_dial.impl != NULL || client->websocket_initialized)
    return SALTS_EBUSY;
  flowie_mqtt_client_finalize(client);
  return SALTS_OK;
}

void flowie_mqtt_client_destroy(flowie_mqtt_client_t *client) {
  int status;
  if (client == NULL) return;
  status = flowie_mqtt_client_try_destroy(client,
      client->timeout_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)client->timeout_ms);
  if (status != SALTS_OK && flowie_mqtt_client_current != client) {
    /* Explicit diagnostic; legacy void destruction never force-frees a
     * retained native context. Use try_destroy to regain error/ownership. */
    fprintf(stderr, "flowie_mqtt_client_destroy: incomplete Owner drain (%d); "
                    "Client storage retained; retry try_destroy()\n", status);
  }
}

static int flowie_mqtt_client_remote_host_valid(const char *host) {
  size_t length;
  if (host == NULL || host[0] == '\0') return 0;
  length = strlen(host);
  if (length > 255u) return 0;
  for (size_t index = 0u; index < length; ++index) {
    const unsigned char c = (unsigned char)host[index];
    if (c <= 32u || c == 127u || c == '/' || c == '\\' ||
        c == '@' || c == '?' || c == '#')
      return 0;
  }
  return 1;
}

static cnet_destination_policy_kind flowie_mqtt_client_destination_map(
    flowie_mqtt_client_destination_kind_t kind) {
  switch (kind) {
  case FLOWIE_MQTT_DESTINATION_EXPLICIT: return CNET_DESTINATION_EXPLICIT;
  case FLOWIE_MQTT_DESTINATION_ROUND_ROBIN: return CNET_DESTINATION_ROUND_ROBIN;
  case FLOWIE_MQTT_DESTINATION_WEIGHTED_ROUND_ROBIN: return CNET_DESTINATION_WEIGHTED_RR;
  case FLOWIE_MQTT_DESTINATION_LEAST_INFLIGHT: return CNET_DESTINATION_LEAST_INFLIGHT;
  case FLOWIE_MQTT_DESTINATION_STRICT_KEY: return CNET_DESTINATION_STRICT_KEY;
  default: return (cnet_destination_policy_kind)0;
  }
}

/* Destination selection is an advisory CNet decision, not a transport or
 * MQTT protocol action. Pin the first winner for all reconnect episodes. */
static int flowie_mqtt_client_choose_destination(flowie_mqtt_client_t *client) {
  cnet_destination_hint hints[FLOWIE_MQTT_CLIENT_MAX_DESTINATIONS] = {{0}};
  cnet_destination_selection selection = {0};
  cnet_destination_result result = {0};
  int rc;
  if (client == NULL) return SALTS_EINVAL;
  if (client->destination_count == 0u) return SALTS_OK;
  for (size_t index = 0u; index < client->destination_count; ++index) {
    const flowie_mqtt_owned_endpoint_t *endpoint = &client->destinations[index];
    hints[index] = (cnet_destination_hint){
        .endpoint_id = endpoint->endpoint_id,
        .weight = endpoint->weight,
        .inflight = endpoint->inflight,
        .eligible = endpoint->eligible != 0u};
  }
  selection.size = sizeof(selection);
  selection.version = CNET_DESTINATION_POLICY_VERSION;
  selection.kind = client->selected_destination_index == SIZE_MAX
                       ? flowie_mqtt_client_destination_map(client->destination_kind)
                       : CNET_DESTINATION_EXPLICIT;
  selection.endpoints = hints;
  selection.endpoint_count = client->destination_count;
  selection.snapshot_generation = client->destination_generation;
  selection.expires_at_ms = client->destination_expires_at_ms;
  selection.now_ms = cmeta_monotonic_ms();
  selection.sequence = client->destination_sequence;
  selection.explicit_endpoint_id =
      client->selected_destination_index == SIZE_MAX
          ? client->destination_explicit_id
          : client->destinations[client->selected_destination_index].endpoint_id;
  selection.key_hash = client->destination_key_hash;
  selection.key_known = client->destination_key_known != 0u;
  rc = cnet_destination_choose(&selection, &result);
  if (rc != SALTS_OK) return rc;
  if (result.index >= client->destination_count ||
      (client->selected_destination_index != SIZE_MAX &&
       result.index != client->selected_destination_index))
    return SALTS_EPROTO;
  client->selected_destination_index = result.index;
  return SALTS_OK;
}

int flowie_mqtt_client_set_destination_policy(
    flowie_mqtt_client_t *client,
    const flowie_mqtt_client_destination_policy_t *policy) {
  cnet_destination_hint hints[FLOWIE_MQTT_CLIENT_MAX_DESTINATIONS] = {{0}};
  flowie_mqtt_owned_endpoint_t *owned;
  int rc;
  if (client == NULL || policy == NULL) return SALTS_EINVAL;
  if (flowie_mqtt_client_is_websocket(client)) return SALTS_ENOTSUP;
  if (policy->size != sizeof(*policy) ||
      policy->version != FLOWIE_MQTT_CLIENT_DESTINATION_VERSION ||
      policy->endpoints == NULL || policy->endpoint_count == 0u ||
      policy->endpoint_count > FLOWIE_MQTT_CLIENT_MAX_DESTINATIONS ||
      policy->snapshot_generation == 0u || policy->expires_at_ms == 0u ||
      flowie_mqtt_client_destination_map(policy->kind) == 0 ||
      (policy->kind == FLOWIE_MQTT_DESTINATION_EXPLICIT &&
       policy->explicit_endpoint_id == 0u) ||
      (policy->kind == FLOWIE_MQTT_DESTINATION_STRICT_KEY && !policy->key_known))
    return SALTS_EINVAL;
  for (size_t index = 0u; index < policy->endpoint_count; ++index) {
    const flowie_mqtt_client_remote_endpoint_t *endpoint = &policy->endpoints[index];
    if (!flowie_mqtt_client_remote_host_valid(endpoint->host) || endpoint->port == 0u)
      return SALTS_EINVAL;
    hints[index] = (cnet_destination_hint){
        .endpoint_id = endpoint->endpoint_id, .weight = endpoint->weight,
        .inflight = endpoint->inflight, .eligible = endpoint->eligible != 0u};
  }
  rc = cnet_destination_validate(hints, policy->endpoint_count);
  if (rc != SALTS_OK) return rc;
  owned = (flowie_mqtt_owned_endpoint_t *)calloc(policy->endpoint_count, sizeof(*owned));
  if (owned == NULL) return SALTS_ENOMEM;
  for (size_t index = 0u; index < policy->endpoint_count; ++index) {
    const flowie_mqtt_client_remote_endpoint_t *endpoint = &policy->endpoints[index];
    owned[index] = (flowie_mqtt_owned_endpoint_t){
        .host = tstr_dup(endpoint->host), .endpoint_id = endpoint->endpoint_id,
        .port = endpoint->port, .weight = endpoint->weight,
        .inflight = endpoint->inflight, .eligible = endpoint->eligible};
    if (owned[index].host == NULL) {
      rc = SALTS_ENOMEM;
      goto reject;
    }
  }
  cmeta_mutex_lock(&client->command_mutex);
  if (client->stopping) {
    rc = SALTS_ESHUTDOWN;
  } else if (client->version_locked || client->destinations != NULL ||
             !deque_empty(&client->commands)) {
    rc = SALTS_EBUSY;
  } else {
    client->destinations = owned;
    client->destination_count = policy->endpoint_count;
    client->destination_kind = policy->kind;
    client->destination_generation = policy->snapshot_generation;
    client->destination_expires_at_ms = policy->expires_at_ms;
    client->destination_sequence = policy->sequence;
    client->destination_explicit_id = policy->explicit_endpoint_id;
    client->destination_key_hash = policy->key_hash;
    client->destination_key_known = policy->key_known;
    rc = SALTS_OK;
  }
  cmeta_mutex_unlock(&client->command_mutex);
  if (rc == SALTS_OK) return SALTS_OK;
reject:
  for (size_t index = 0u; index < policy->endpoint_count; ++index)
    tstr_freep(&owned[index].host);
  free(owned);
  return rc;
}

static int flowie_mqtt_client_uri(flowie_mqtt_client_t *client, const char *scheme,
                                  const char *path, char **out) {
  int bracket_host;
  size_t scheme_size;
  size_t host_size;
  size_t path_size;
  size_t capacity;
  int written;
  const char *dial_host;
  int dial_port;
  if (!client || !scheme || !out) return SALTS_EINVAL;
  dial_host = client->host;
  dial_port = client->port;
  if (client->destination_count != 0u) {
    if (client->selected_destination_index >= client->destination_count) return SALTS_EINVAL;
    const flowie_mqtt_owned_endpoint_t *endpoint =
        &client->destinations[client->selected_destination_index];
    dial_host = endpoint->host;
    dial_port = endpoint->port;
  }
  bracket_host = strchr(dial_host, ':') != NULL && dial_host[0] != '[';
  scheme_size = strlen(scheme);
  host_size = strlen(dial_host);
  path_size = path ? strlen(path) : 0u;
  *out = NULL;
  if (scheme_size > SIZE_MAX - host_size - path_size - 32u) return SALTS_EMSGSIZE;
  capacity = scheme_size + host_size + path_size + 32u;
  *out = (char *)malloc(capacity);
  if (!*out) return SALTS_ENOMEM;
  written = bracket_host
                ? snprintf(*out, capacity, "%s://[%s]:%d%s", scheme, dial_host,
                           dial_port, path ? path : "")
                : snprintf(*out, capacity, "%s://%s:%d%s", scheme, dial_host,
                           dial_port, path ? path : "");
  if (written < 0 || (size_t)written >= capacity) {
    free(*out);
    *out = NULL;
    return SALTS_EMSGSIZE;
  }
  return SALTS_OK;
}

/* CNet classifies the failed physical connection; MQTT packet/session replay
 * is not authorized by this decision. A TLS handshake failure is a security
 * failure even when the underlying TLS provider reports generic EIO. */
static cnet_reconnect_failure_kind flowie_mqtt_client_dial_classify(
    void *user, cnet_connection_state state, const cnet_error *error) {
  flowie_mqtt_client_t *client = (flowie_mqtt_client_t *)user;
  if (client == NULL || state != CNET_CONNECTION_FAILED || error == NULL)
    return CNET_RECONNECT_PERMANENT;
  if (client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_TLS &&
      error->stage != NULL && strcmp(error->stage, "handshake") == 0) {
    client->network_security_failure = 1;
    return CNET_RECONNECT_SECURITY;
  }
  return flowie_mqtt_client_reconnect_status(error->status)
             ? CNET_RECONNECT_TRANSIENT : CNET_RECONNECT_PERMANENT;
}

static int flowie_mqtt_client_transport_connect(flowie_mqtt_client_t *client) {
  char *uri = NULL;
  int rc;
  if (!client) return SALTS_EINVAL;
  if (flowie_mqtt_client_is_websocket(client)) {
    /* A retained CHttp owner must be destroyed, never initialized over. */
    if (client->websocket.impl != NULL) return SALTS_EBUSY;
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
  if (!client->network_initialized || !client->network_manager_initialized)
    return SALTS_EINVAL;
  if (client->network_dial_initialized) return SALTS_EBUSY;
  rc = flowie_mqtt_client_choose_destination(client);
  if (rc != SALTS_OK) return rc;
  rc = flowie_mqtt_client_uri(
      client, client->transport == FLOWIE_MQTT_CLIENT_TRANSPORT_TLS ? "tls" : "tcp",
      NULL, &uri);
  if (rc != SALTS_OK) return rc;
  {
    cnet_managed_dial_config config = {0};
    cnet_managed_dial_snapshot snapshot = {0};
    cnet_manager_snapshot manager_snapshot = {0};
    uint64_t now = cmeta_monotonic_ms();
    uint64_t span = client->timeout_ms > UINT64_MAX / 3u
                        ? UINT64_MAX : client->timeout_ms * 3u;
    uint64_t deadline = span > UINT64_MAX - now ? UINT64_MAX : now + span;
    uint64_t wait_ms = 0u;
    rc = cnet_manager_get_snapshot(&client->network_manager, &manager_snapshot);
    if (rc == SALTS_OK && !manager_snapshot.drained) rc = SALTS_EBUSY;
    if (rc == SALTS_OK && deadline <= now) rc = SALTS_ETIMEDOUT;
    if (rc != SALTS_OK) {
      free(uri);
      return rc;
    }
    client->network_connected = 0;
    client->network_terminal = 0;
    client->network_status = SALTS_OK;
    client->network_security_failure = 0;
    client->network_connection = (cnet_connection){0};
    /* CNet borrows TLS profile pointers through Managed Dial destroy. All
     * paths reference client-owned, deep-copied identity and trust strings. */
    client->network_dial_tls = (cnet_tls_client_config){
        .size = sizeof(client->network_dial_tls),
        .ca_file = client->tls_ca_file,
        .cert_file = client->tls_cert_file,
        .key_file = client->tls_key_file,
        .key_password = client->tls_key_password,
        .server_name = client->host};
    config.size = sizeof(config);
    config.version = CNET_MANAGED_DIAL_VERSION;
    config.manager = &client->network_manager;
    config.client = &client->network;
    config.connection = (cnet_connect_options){
        .uri = uri,
        .tls = client->tls_configured ? &client->network_dial_tls : NULL,
        .observer = {.on_state = flowie_mqtt_client_network_state,
                     .on_receive = flowie_mqtt_client_network_receive,
                     .on_send = flowie_mqtt_client_network_send,
                     .user = client}};
    /* One CNet physical attempt per MQTT CONNECT, never implicit packet
     * replay. Flowie's existing optional MQTT reconnect policy remains the
     * only authority for another logical CONNECT command. */
    config.recovery = (cnet_reconnect_config){
        .size = sizeof(cnet_reconnect_config),
        .version = CNET_RECOVERY_POLICY_VERSION,
        .max_attempts = 1u,
        .deadline_ms = deadline,
        .initial_backoff_ms = 0u,
        .maximum_backoff_ms = 0u,
        .jitter_seed = 1u};
    config.recovery_episode_ms = span;
    config.classify = flowie_mqtt_client_dial_classify;
    config.classify_user = client;
    rc = cnet_managed_dial_init(&client->network_dial, &config);
    if (rc == SALTS_OK) {
      client->network_dial_initialized = 1;
      rc = cnet_managed_dial_advance(&client->network_dial, now, &wait_ms);
    }
    if (rc == SALTS_OK) {
      rc = cnet_managed_dial_get_snapshot(&client->network_dial, &snapshot);
      if (rc == SALTS_OK) client->network_connection = snapshot.connection;
    }
    free(uri);
    if (rc != SALTS_OK) return rc;
    /* CNet CONNECTED still is not MQTT-ready. The ticket will only be
     * accepted after AUTH/CONNACK and protocol negotiation succeed. */
    while (!client->network_connected && !client->network_terminal) {
      size_t events = 0u;
      if (flowie_mqtt_client_is_stopping(client)) return SALTS_ESHUTDOWN;
      if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
      rc = flowie_mqtt_client_poll_managed(
          client, flowie_mqtt_client_poll_slice(deadline), &events);
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
  cmeta_bytes_reset(&client->framing);
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
  if (!flowie_mqtt_client_is_websocket(client) && client->network_dial_initialized) {
    cnet_managed_dial_snapshot snapshot = {0};
    rc = cnet_managed_dial_get_snapshot(&client->network_dial, &snapshot);
    if (rc == SALTS_OK)
      rc = cnet_managed_dial_protocol_ready(
          &client->network_dial, snapshot.recovery_ticket, cmeta_monotonic_ms());
    if (rc != SALTS_OK) goto fail;
  }
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
  cmeta_mutex_lock(&client->command_mutex);
  if (client->stopping) rc = SALTS_ESHUTDOWN;
  else if (client->version_locked) rc = SALTS_EALREADY;
  else {
    client->selected_version = version;
    rc = SALTS_OK;
  }
  cmeta_mutex_unlock(&client->command_mutex);
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
