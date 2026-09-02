#include "flowie_mqtt_client.h"

#include "flowie_mqtt_protocol.h"
#include "flowie_test_socket.h"
#include "mtls_test_server.h"
#include "tls_test_pki.h"

#include "platform.h"
#include "CoroNet/turbo_coro_socket.h"
#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#define FLOWIE_MQTT_CLIENT_TEST_BUFFER_SIZE 4096u
#define FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS 5000u
#define FLOWIE_MQTT_CLIENT_TEST_CLOSE_GRACE_MS 10u
#define FLOWIE_MQTT_CLIENT_TEST_CALLBACK_DISCONNECT_GRACE_MS 50u
#define FLOWIE_MQTT_CLIENT_TEST_QOS2_WAKE_ITERATIONS 64u

typedef struct flowie_mqtt_test_broker_stream_s {
  coro_socket_t *socket;
  uint8_t bytes[FLOWIE_MQTT_CLIENT_TEST_BUFFER_SIZE];
  size_t size;
  size_t pending_size;
} flowie_mqtt_test_broker_stream_t;

typedef struct flowie_mqtt_test_state_s {
  coro_context_t *context;
  flowie_mqtt_client_t *client;
  flowie_mqtt_version_t version;
  int server_rc;
  int server_done;
  int publish_count;
  int secondary_match_count;
  char topics[2][32];
  char payloads[2][32];
  int expect_disconnect;
  atomic_int completions;
  atomic_int publish_completions;
  atomic_int completion_error;
  atomic_int background_error;
} flowie_mqtt_test_state_t;

typedef struct flowie_mqtt_test_shutdown_s {
  int count;
  int statuses[2];
} flowie_mqtt_test_shutdown_t;

typedef struct flowie_mqtt_reentrant_shutdown_s {
  atomic_int connect_done;
  atomic_int connect_status;
  atomic_int enqueue_status;
  atomic_int ping_received;
  atomic_int ping_count;
  atomic_int ping_status;
} flowie_mqtt_reentrant_shutdown_t;

typedef struct flowie_mqtt_test_error_state_s {
  atomic_int connect_done;
  atomic_int connect_status;
  atomic_int error_count;
  atomic_int error_status;
  int server_status;
  int server_done;
} flowie_mqtt_test_error_state_t;

typedef struct flowie_mqtt_mtls_state_s {
  atomic_int done;
  atomic_int status;
} flowie_mqtt_mtls_state_t;

typedef struct flowie_mqtt_limits_state_s {
  atomic_int connect_done;
  atomic_int connect_status;
  atomic_int publish_count;
  atomic_int disconnect_done;
  int publish_status[4];
  int server_status;
  int server_done;
} flowie_mqtt_limits_state_t;

typedef struct flowie_mqtt_auth_state_s {
  atomic_int connect_done;
  atomic_int connect_status;
  atomic_int auth_done;
  atomic_int auth_status;
  atomic_int disconnect_done;
  atomic_int challenge_count;
  uint8_t initial_response[64];
  size_t initial_response_size;
  uint8_t reauth_response[64];
  size_t reauth_response_size;
  int fail_challenge;
  int fail_challenge_index;
  int server_status;
  int server_done;
} flowie_mqtt_auth_state_t;

typedef struct flowie_mqtt_qos2_wake_state_s {
  atomic_int connect_done;
  atomic_int connect_status;
  atomic_int publish_count;
  atomic_int publish_status;
  atomic_int disconnect_done;
  atomic_int disconnect_status;
  atomic_int inbound_count;
  atomic_int pulse_count;
  atomic_int server_status;
  atomic_int server_done;
  unsigned int pubrec_sent;
  unsigned int pubrel_received;
  unsigned int pubcomp_sent;
} flowie_mqtt_qos2_wake_state_t;

typedef struct flowie_mqtt_resilience_state_s {
  atomic_int broker_connects;
  atomic_int initial_completions;
  atomic_int initial_reason;
  atomic_int reconnect_completions;
  atomic_int reconnect_attempt;
  atomic_int reconnect_status;
  atomic_int reconnect_reason;
  atomic_int server_status;
  atomic_int refresh_calls;
  atomic_int refreshed_token_seen;
  atomic_int background_errors;
  atomic_int background_status;
  atomic_int ping_submit_status;
  atomic_int ping_completions;
  atomic_int ping_status;
  uint8_t disconnect_reason;
} flowie_mqtt_resilience_state_t;

static void flowie_mqtt_resilience_state_init(flowie_mqtt_resilience_state_t *state) {
  memset(state, 0, sizeof(*state));
  atomic_init(&state->broker_connects, 0);
  atomic_init(&state->initial_completions, 0);
  atomic_init(&state->initial_reason, -1);
  atomic_init(&state->reconnect_completions, 0);
  atomic_init(&state->reconnect_attempt, 0);
  atomic_init(&state->reconnect_status, TURBO_EBUSY);
  atomic_init(&state->reconnect_reason, -1);
  atomic_init(&state->server_status, TURBO_OK);
  atomic_init(&state->refresh_calls, 0);
  atomic_init(&state->refreshed_token_seen, 0);
  atomic_init(&state->background_errors, 0);
  atomic_init(&state->background_status, TURBO_OK);
  atomic_init(&state->ping_submit_status, TURBO_EBUSY);
  atomic_init(&state->ping_completions, 0);
  atomic_init(&state->ping_status, TURBO_OK);
}

static void
flowie_mqtt_test_error_connect_completion(flowie_mqtt_client_t *client, int status,
                                          const flowie_mqtt_control_packet_view_t *response,
                                          void *user_data);
static void flowie_mqtt_test_background_error(flowie_mqtt_client_t *client, int status,
                                              void *user_data);

static void flowie_mqtt_test_ignore_completion(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user_data) {
  (void)client;
  (void)status;
  (void)response;
  (void)user_data;
}

static void flowie_mqtt_mtls_connect_completion(flowie_mqtt_client_t *client, int status,
                                                const flowie_mqtt_control_packet_view_t *response,
                                                void *user_data) {
  flowie_mqtt_mtls_state_t *state = (flowie_mqtt_mtls_state_t *)user_data;
  (void)client;
  if (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK) status = TURBO_EPROTO;
  atomic_store_explicit(&state->status, status, memory_order_relaxed);
  atomic_store_explicit(&state->done, 1, memory_order_release);
}

static int flowie_mqtt_tls_rejection_case(const char *host, const char *ca_file,
                                          const char *cert_file, const char *key_file) {
  static const uint8_t connack[] = {0x20u, 0x03u, 0x00u, 0x00u, 0x00u};
  static const uint8_t client_id[] = "flowie-tls-rejected";
  flow_mtls_test_server_t server;
  flowie_mqtt_test_error_state_t state;
  flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_t *client = NULL;
  uint64_t deadline;
  int rc;
  memset(&state, 0, sizeof(state));
  atomic_init(&state.connect_done, 0);
  atomic_init(&state.connect_status, TURBO_EBUSY);
  atomic_init(&state.error_count, 0);
  atomic_init(&state.error_status, TURBO_OK);
  if (flow_mtls_test_server_start(&server, connack, sizeof(connack)) != 0) return TURBO_EIO;
  config.host = host;
  config.port = server.port;
  config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
  config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
  config.tls.ca_file = ca_file;
  config.tls.cert_file = cert_file;
  config.tls.key_file = key_file;
  config.on_connect = flowie_mqtt_test_error_connect_completion;
  config.on_error = flowie_mqtt_test_background_error;
  config.user_data = &state;
  rc = flowie_mqtt_client_create(&config, &client);
  if (rc == TURBO_OK) {
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    rc = flowie_mqtt_client_connect(client, &connect);
  }
  deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
  while (rc == TURBO_OK && !atomic_load_explicit(&state.connect_done, memory_order_acquire) &&
         turbo_monotonic_ms() < deadline)
    turbo_sleep_ms(1u);
  flow_mtls_test_server_join(&server);
  if (rc == TURBO_OK && !atomic_load_explicit(&state.connect_done, memory_order_acquire))
    rc = TURBO_ETIMEDOUT;
  if (rc == TURBO_OK &&
      atomic_load_explicit(&state.connect_status, memory_order_relaxed) == TURBO_OK)
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK && server.status == 0) rc = TURBO_EPROTO;
  if (rc == TURBO_OK && flowie_mqtt_client_is_connected(client)) rc = TURBO_EPROTO;
  flowie_mqtt_client_destroy(client);
  return rc;
}

static int flowie_mqtt_test_next_packet(flowie_mqtt_test_broker_stream_t *stream,
                                        flowie_mqtt_version_t version,
                                        flowie_mqtt_packet_view_t *out) {
  flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  if (!stream || !stream->socket || !out) return TURBO_EINVAL;
  if (stream->pending_size != 0u) {
    if (stream->pending_size > stream->size) return TURBO_EPROTO;
    memmove(stream->bytes, stream->bytes + stream->pending_size,
            stream->size - stream->pending_size);
    stream->size -= stream->pending_size;
    stream->pending_size = 0u;
  }
  options.version = version;
  options.max_packet_size = sizeof(stream->bytes);
  for (;;) {
    flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
    size_t consumed = 0u;
    int rc;
    if (stream->size != 0u) {
      rc =
          flowie_mqtt_packet_parse(stream->bytes, stream->size, &options, &packet, &consumed, NULL);
      if (rc == FLOWIE_MQTT_PARSE_OK) {
        if (consumed == 0u || consumed > stream->size) return TURBO_EPROTO;
        stream->pending_size = consumed;
        *out = packet;
        return TURBO_OK;
      }
      if (rc != FLOWIE_MQTT_PARSE_NEED_MORE) return TURBO_EPROTO;
    }
    {
      char *received = NULL;
      size_t received_size = 0u;
      rc = coro_socket_recv(stream->socket, &received, &received_size);
      if (rc != TURBO_OK) return rc;
      if (!received || received_size == 0u ||
          received_size > sizeof(stream->bytes) - stream->size) {
        if (received) coro_socket_free_recv(received);
        return TURBO_ECONNRESET;
      }
      memcpy(stream->bytes + stream->size, received, received_size);
      stream->size += received_size;
      coro_socket_free_recv(received);
    }
  }
}

static int flowie_mqtt_test_send_control(coro_socket_t *socket, flowie_mqtt_version_t version,
                                         flowie_mqtt_packet_type_t type, uint16_t packet_id,
                                         uint8_t reason_code, flowie_mqtt_span_t reason_codes) {
  flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  uint8_t encoded[64];
  size_t written = 0u;
  int rc;
  packet.version = version;
  packet.type = type;
  packet.packet_id = packet_id;
  packet.reason_code = reason_code;
  packet.reason_codes = reason_codes;
  rc = flowie_mqtt_control_packet_encode(&packet, encoded, sizeof(encoded), &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return TURBO_EPROTO;
  return coro_socket_send(socket, (const char *)encoded, written);
}

static int flowie_mqtt_test_send_control_properties(coro_socket_t *socket,
                                                    flowie_mqtt_packet_type_t type,
                                                    uint8_t reason_code,
                                                    flowie_mqtt_span_t properties) {
  enum { FLOWIE_MQTT_CLIENT_TEST_CONTROL_CAPACITY = 256u };
  flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  uint8_t encoded[FLOWIE_MQTT_CLIENT_TEST_CONTROL_CAPACITY];
  size_t written = 0u;
  int rc;
  packet.version = FLOWIE_MQTT_VERSION_5;
  packet.type = type;
  packet.reason_code = reason_code;
  packet.properties = properties;
  rc = flowie_mqtt_control_packet_encode(&packet, encoded, sizeof(encoded), &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return TURBO_EPROTO;
  return coro_socket_send(socket, (const char *)encoded, written);
}

static int flowie_mqtt_test_send_connack_properties(coro_socket_t *socket,
                                                    flowie_mqtt_span_t properties) {
  return flowie_mqtt_test_send_control_properties(socket, FLOWIE_MQTT_PACKET_CONNACK, 0u,
                                                  properties);
}

static int flowie_mqtt_test_auth_properties_encode(const char *method, const char *data,
                                                   uint8_t *output, size_t capacity,
                                                   size_t *written) {
  size_t method_size;
  size_t data_size;
  size_t offset = 0u;
  if (!method || !method[0] || !data || !output || !written) return TURBO_EINVAL;
  method_size = strlen(method);
  data_size = strlen(data);
  if (method_size > UINT16_MAX || data_size > UINT16_MAX ||
      method_size > SIZE_MAX - data_size - 6u || capacity < method_size + data_size + 6u)
    return TURBO_ENOSPC;
  output[offset++] = FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD;
  output[offset++] = (uint8_t)(method_size >> 8u);
  output[offset++] = (uint8_t)method_size;
  memcpy(output + offset, method, method_size);
  offset += method_size;
  output[offset++] = FLOWIE_MQTT_PROPERTY_AUTHENTICATION_DATA;
  output[offset++] = (uint8_t)(data_size >> 8u);
  output[offset++] = (uint8_t)data_size;
  memcpy(output + offset, data, data_size);
  offset += data_size;
  *written = offset;
  return TURBO_OK;
}

static int
flowie_mqtt_test_auth_properties_match(const flowie_mqtt_property_block_view_t *properties,
                                       const char *method, const char *data) {
  flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
  flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
  int method_seen = 0;
  int data_seen = 0;
  int rc;
  if (!properties || !method || !data) return 0;
  rc = flowie_mqtt_property_iterator_init(properties, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) return 0;
  while ((rc = flowie_mqtt_property_iterator_next(&iterator, &property)) == FLOWIE_MQTT_PARSE_OK) {
    if (property.identifier == FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD) {
      if (property.value.size != strlen(method) ||
          memcmp(property.value.data, method, property.value.size) != 0)
        return 0;
      method_seen = 1;
    } else if (property.identifier == FLOWIE_MQTT_PROPERTY_AUTHENTICATION_DATA) {
      if (property.value.size != strlen(data) ||
          memcmp(property.value.data, data, property.value.size) != 0)
        return 0;
      data_seen = 1;
    }
  }
  return rc == FLOWIE_MQTT_PARSE_NEED_MORE && method_seen && data_seen;
}

static int flowie_mqtt_test_expect_type(flowie_mqtt_test_broker_stream_t *stream,
                                        flowie_mqtt_version_t version,
                                        flowie_mqtt_packet_type_t expected,
                                        flowie_mqtt_packet_view_t *packet) {
  int rc = flowie_mqtt_test_next_packet(stream, version, packet);
  if (rc != TURBO_OK) return rc;
  return packet->type == expected ? TURBO_OK : TURBO_EPROTO;
}

static int flowie_mqtt_test_send_publish(flowie_mqtt_test_broker_stream_t *stream,
                                         flowie_mqtt_version_t version, uint16_t packet_id,
                                         flowie_mqtt_span_t topic, flowie_mqtt_span_t payload) {
  flowie_mqtt_publish_packet_t publish = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  uint8_t encoded[128];
  size_t written = 0u;
  int rc;
  publish.version = version;
  publish.qos = 1u;
  publish.packet_id = packet_id;
  publish.topic = topic;
  publish.payload = payload;
  rc = flowie_mqtt_publish_packet_encode(&publish, encoded, sizeof(encoded), &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return TURBO_EPROTO;
  rc = coro_socket_send(stream->socket, (const char *)encoded, written);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_test_expect_type(stream, version, FLOWIE_MQTT_PACKET_PUBACK, &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_control_packet_parse(&packet, &control);
  return rc == FLOWIE_MQTT_PARSE_OK && control.packet_id == packet_id ? TURBO_OK : TURBO_EPROTO;
}

static int flowie_mqtt_test_send_qos0_publish(coro_socket_t *socket, flowie_mqtt_span_t topic,
                                              flowie_mqtt_span_t payload) {
  flowie_mqtt_publish_packet_t publish = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
  uint8_t encoded[128];
  size_t written = 0u;
  int rc;
  publish.version = FLOWIE_MQTT_VERSION_5;
  publish.qos = 0u;
  publish.topic = topic;
  publish.payload = payload;
  rc = flowie_mqtt_publish_packet_encode(&publish, encoded, sizeof(encoded), &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return TURBO_EPROTO;
  return coro_socket_send(socket, (const char *)encoded, written);
}

static int flowie_mqtt_test_broker_run(coro_socket_t *socket, flowie_mqtt_test_state_t *state) {
  static const uint8_t inbound_topics[2][17] = {"server/topic/one", "server/topic/two"};
  static const uint8_t inbound_payloads[2][16] = {"from-broker-one", "from-broker-two"};
  static const uint8_t outbound_topics[2][17] = {"client/topic/one", "client/topic/two"};
  static const uint8_t outbound_payloads[2][16] = {"from-client-one", "from-client-two"};
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  flowie_mqtt_subscribe_view_t subscribe = FLOWIE_MQTT_SUBSCRIBE_VIEW_INIT;
  flowie_mqtt_publish_view_t publish = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
  flowie_mqtt_unsubscribe_view_t unsubscribe = FLOWIE_MQTT_UNSUBSCRIBE_VIEW_INIT;
  flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  uint8_t granted_qos[2] = {1u, 1u};
  uint8_t unsubscribe_reason[2] = {0u, 0u};
  flowie_mqtt_span_t subscribe_reasons = {granted_qos, 2u};
  flowie_mqtt_span_t unsubscribe_reasons = {unsubscribe_reason, 2u};
  uint16_t packet_id;
  int rc;

  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);

  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_connect_parse(&packet, &connect);
  if (rc != FLOWIE_MQTT_PARSE_OK || connect.version != state->version) return TURBO_EPROTO;
  rc = flowie_mqtt_test_send_control(socket, state->version, FLOWIE_MQTT_PACKET_CONNACK, 0u, 0u,
                                     (flowie_mqtt_span_t){0});
  if (rc != TURBO_OK) return rc;

  rc = flowie_mqtt_test_expect_type(&stream, state->version, FLOWIE_MQTT_PACKET_SUBSCRIBE, &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_subscribe_parse(&packet, &subscribe);
  if (rc != FLOWIE_MQTT_PARSE_OK || subscribe.packet_id != 1u || subscribe.entry_count != 2u)
    return TURBO_EPROTO;
  rc = flowie_mqtt_test_send_control(socket, state->version, FLOWIE_MQTT_PACKET_SUBACK,
                                     subscribe.packet_id, 0u, subscribe_reasons);
  if (rc != TURBO_OK) return rc;

  packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
  rc = flowie_mqtt_test_expect_type(&stream, state->version, FLOWIE_MQTT_PACKET_PUBLISH, &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_publish_parse(&packet, &publish);
  if (rc != FLOWIE_MQTT_PARSE_OK || publish.qos != 1u || publish.packet_id != 2u ||
      publish.topic.size != sizeof(outbound_topics[0]) - 1u ||
      memcmp(publish.topic.data, outbound_topics[0], publish.topic.size) != 0 ||
      publish.payload.size != sizeof(outbound_payloads[0]) - 1u ||
      memcmp(publish.payload.data, outbound_payloads[0], publish.payload.size) != 0)
    return TURBO_EPROTO;
  rc = flowie_mqtt_test_send_publish(
      &stream, state->version, 77u,
      (flowie_mqtt_span_t){inbound_topics[0], sizeof(inbound_topics[0]) - 1u},
      (flowie_mqtt_span_t){inbound_payloads[0], sizeof(inbound_payloads[0]) - 1u});
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_test_send_control(socket, state->version, FLOWIE_MQTT_PACKET_PUBACK,
                                     publish.packet_id, 0u, (flowie_mqtt_span_t){0});
  if (rc != TURBO_OK) return rc;

  packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
  publish = (flowie_mqtt_publish_view_t)FLOWIE_MQTT_PUBLISH_VIEW_INIT;
  rc = flowie_mqtt_test_expect_type(&stream, state->version, FLOWIE_MQTT_PACKET_PUBLISH, &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_publish_parse(&packet, &publish);
  if (rc != FLOWIE_MQTT_PARSE_OK || publish.qos != 2u || publish.packet_id != 3u ||
      publish.topic.size != sizeof(outbound_topics[1]) - 1u ||
      memcmp(publish.topic.data, outbound_topics[1], publish.topic.size) != 0 ||
      publish.payload.size != sizeof(outbound_payloads[1]) - 1u ||
      memcmp(publish.payload.data, outbound_payloads[1], publish.payload.size) != 0)
    return TURBO_EPROTO;
  packet_id = publish.packet_id;
  rc = flowie_mqtt_test_send_publish(
      &stream, state->version, 78u,
      (flowie_mqtt_span_t){inbound_topics[1], sizeof(inbound_topics[1]) - 1u},
      (flowie_mqtt_span_t){inbound_payloads[1], sizeof(inbound_payloads[1]) - 1u});
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_test_send_control(socket, state->version, FLOWIE_MQTT_PACKET_PUBREC, packet_id,
                                     0u, (flowie_mqtt_span_t){0});
  if (rc != TURBO_OK) return rc;
  packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
  control = (flowie_mqtt_control_packet_view_t)FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  rc = flowie_mqtt_test_expect_type(&stream, state->version, FLOWIE_MQTT_PACKET_PUBREL, &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_control_packet_parse(&packet, &control);
  if (rc != FLOWIE_MQTT_PARSE_OK || control.packet_id != packet_id) return TURBO_EPROTO;
  rc = flowie_mqtt_test_send_control(socket, state->version, FLOWIE_MQTT_PACKET_PUBCOMP, packet_id,
                                     0u, (flowie_mqtt_span_t){0});
  if (rc != TURBO_OK) return rc;

  packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
  rc = flowie_mqtt_test_expect_type(&stream, state->version, FLOWIE_MQTT_PACKET_UNSUBSCRIBE,
                                    &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_unsubscribe_parse(&packet, &unsubscribe);
  if (rc != FLOWIE_MQTT_PARSE_OK || unsubscribe.packet_id != 4u || unsubscribe.filter_count != 2u)
    return TURBO_EPROTO;
  rc = flowie_mqtt_test_send_control(
      socket, state->version, FLOWIE_MQTT_PACKET_UNSUBACK, unsubscribe.packet_id, 0u,
      state->version == FLOWIE_MQTT_VERSION_5 ? unsubscribe_reasons : (flowie_mqtt_span_t){0});
  if (rc != TURBO_OK) return rc;

  packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
  rc = flowie_mqtt_test_expect_type(&stream, state->version, FLOWIE_MQTT_PACKET_PINGREQ, &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_test_send_control(socket, state->version, FLOWIE_MQTT_PACKET_PINGRESP, 0u, 0u,
                                     (flowie_mqtt_span_t){0});
  if (rc != TURBO_OK) return rc;

  if (!state->expect_disconnect) {
    coro_sleep(coro_context_current(), FLOWIE_MQTT_CLIENT_TEST_CALLBACK_DISCONNECT_GRACE_MS);
    return TURBO_OK;
  }

  packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
  return flowie_mqtt_test_expect_type(&stream, state->version, FLOWIE_MQTT_PACKET_DISCONNECT,
                                      &packet);
}

static void flowie_mqtt_test_broker_handler(coro_socket_t *socket, void *arg) {
  flowie_mqtt_test_state_t *state = (flowie_mqtt_test_state_t *)arg;
  state->server_rc = flowie_mqtt_test_broker_run(socket, state);
  if (state->server_rc == TURBO_OK)
    coro_sleep(coro_context_current(), FLOWIE_MQTT_CLIENT_TEST_CLOSE_GRACE_MS);
  state->server_done = 1;
}

static void flowie_mqtt_reentrant_shutdown_broker_handler(coro_socket_t *socket, void *arg) {
  flowie_mqtt_reentrant_shutdown_t *state = (flowie_mqtt_reentrant_shutdown_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  if (flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                   FLOWIE_MQTT_PACKET_CONNECT, &packet) != TURBO_OK)
    return;
  if (flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK ||
      connect.version != FLOWIE_MQTT_VERSION_5)
    return;
  if (flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5, FLOWIE_MQTT_PACKET_CONNACK, 0u,
                                    0u, (flowie_mqtt_span_t){0}) != TURBO_OK)
    return;
  if (flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5, FLOWIE_MQTT_PACKET_PINGREQ,
                                   &packet) != TURBO_OK)
    return;
  atomic_store_explicit(&state->ping_received, 1, memory_order_release);

  /* Keep the accepted PING command pending until client destruction owns cancellation. */
  (void)flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                     FLOWIE_MQTT_PACKET_DISCONNECT, &packet);
}

static void flowie_mqtt_qos2_wake_broker_handler(coro_socket_t *socket, void *arg) {
  static const uint8_t inbound_topic[] = "server/qos2/wake";
  static const uint8_t inbound_payload[] = "wake";
  flowie_mqtt_qos2_wake_state_t *state = (flowie_mqtt_qos2_wake_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  flowie_mqtt_publish_view_t publish = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
  flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK &&
      (flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK ||
       connect.version != FLOWIE_MQTT_VERSION_5))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5,
                                       FLOWIE_MQTT_PACKET_CONNACK, 0u, 0u,
                                       (flowie_mqtt_span_t){0});
  for (unsigned int i = 0u;
       rc == TURBO_OK && i < FLOWIE_MQTT_CLIENT_TEST_QOS2_WAKE_ITERATIONS; ++i) {
    if (rc == TURBO_OK)
      rc = flowie_mqtt_test_send_qos0_publish(
          socket, (flowie_mqtt_span_t){inbound_topic, sizeof(inbound_topic) - 1u},
          (flowie_mqtt_span_t){inbound_payload, sizeof(inbound_payload) - 1u});
    if (rc == TURBO_OK)
      rc = flowie_mqtt_test_send_qos0_publish(
          socket, (flowie_mqtt_span_t){inbound_topic, sizeof(inbound_topic) - 1u},
          (flowie_mqtt_span_t){inbound_payload, sizeof(inbound_payload) - 1u});
    packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
    publish = (flowie_mqtt_publish_view_t)FLOWIE_MQTT_PUBLISH_VIEW_INIT;
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                      FLOWIE_MQTT_PACKET_PUBLISH, &packet);
    if (rc == TURBO_OK &&
        (flowie_mqtt_publish_parse(&packet, &publish) != FLOWIE_MQTT_PARSE_OK ||
         publish.qos != 2u || publish.packet_id == 0u))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) {
      rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5,
                                         FLOWIE_MQTT_PACKET_PUBREC, publish.packet_id, 0u,
                                         (flowie_mqtt_span_t){0});
      if (rc == TURBO_OK) ++state->pubrec_sent;
    }
    packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
    control = (flowie_mqtt_control_packet_view_t)FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
    if (rc == TURBO_OK)
      rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                        FLOWIE_MQTT_PACKET_PUBREL, &packet);
    if (rc == TURBO_OK &&
        (flowie_mqtt_control_packet_parse(&packet, &control) != FLOWIE_MQTT_PARSE_OK ||
         control.packet_id != publish.packet_id))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) ++state->pubrel_received;
    if (rc == TURBO_OK) {
      rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5,
                                         FLOWIE_MQTT_PACKET_PUBCOMP, publish.packet_id, 0u,
                                         (flowie_mqtt_span_t){0});
      if (rc == TURBO_OK) ++state->pubcomp_sent;
    }
  }
  packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                      FLOWIE_MQTT_PACKET_DISCONNECT, &packet);
  atomic_store_explicit(&state->server_status, rc, memory_order_relaxed);
  atomic_store_explicit(&state->server_done, 1, memory_order_release);
}

static void flowie_mqtt_test_context_runner(void *arg) {
  (void)coro_context_run((coro_context_t *)arg, TURBO_RUN_DEFAULT);
}

static int flowie_mqtt_test_on_publish(flowie_mqtt_client_t *client,
                                       const flowie_mqtt_publish_view_t *publish, void *user_data) {
  flowie_mqtt_test_state_t *state = (flowie_mqtt_test_state_t *)user_data;
  int index = state->publish_count;
  (void)client;
  if (!publish || index < 0 || index >= 2 || publish->topic.size >= sizeof(state->topics[index]) ||
      publish->payload.size >= sizeof(state->payloads[index]))
    return TURBO_EMSGSIZE;
  memcpy(state->topics[index], publish->topic.data, publish->topic.size);
  state->topics[index][publish->topic.size] = '\0';
  memcpy(state->payloads[index], publish->payload.data, publish->payload.size);
  state->payloads[index][publish->payload.size] = '\0';
  ++state->publish_count;
  return TURBO_OK;
}

static int flowie_mqtt_test_on_secondary_match(flowie_mqtt_client_t *client,
                                               const flowie_mqtt_publish_view_t *publish,
                                               void *user_data) {
  static const uint8_t expected_topic[] = "server/topic/two";
  flowie_mqtt_test_state_t *state = (flowie_mqtt_test_state_t *)user_data;
  (void)client;
  if (!publish || publish->topic.size != sizeof(expected_topic) - 1u ||
      memcmp(publish->topic.data, expected_topic, sizeof(expected_topic) - 1u) != 0)
    return TURBO_EPROTO;
  ++state->secondary_match_count;
  return TURBO_OK;
}

static void flowie_mqtt_test_record_completion(flowie_mqtt_test_state_t *state, int status,
                                               const flowie_mqtt_control_packet_view_t *response,
                                               flowie_mqtt_packet_type_t expected_type,
                                               int expects_response) {
  int error = status;
  if (error == TURBO_OK && expects_response && (!response || response->type != expected_type))
    error = TURBO_EPROTO;
  if (error == TURBO_OK && !expects_response && response) error = TURBO_EPROTO;
  if (error != TURBO_OK) {
    int unset = TURBO_OK;
    (void)atomic_compare_exchange_strong_explicit(&state->completion_error, &unset, error,
                                                  memory_order_acq_rel, memory_order_acquire);
  }
  atomic_fetch_add_explicit(&state->completions, 1, memory_order_release);
}

static void flowie_mqtt_test_connect_completion(flowie_mqtt_client_t *client, int status,
                                                const flowie_mqtt_control_packet_view_t *response,
                                                void *user_data) {
  (void)client;
  flowie_mqtt_test_record_completion((flowie_mqtt_test_state_t *)user_data, status, response,
                                     FLOWIE_MQTT_PACKET_CONNACK, 1);
}

static void flowie_mqtt_test_subscribe_completion(flowie_mqtt_client_t *client, int status,
                                                  const flowie_mqtt_control_packet_view_t *response,
                                                  void *user_data) {
  (void)client;
  flowie_mqtt_test_record_completion((flowie_mqtt_test_state_t *)user_data, status, response,
                                     FLOWIE_MQTT_PACKET_SUBACK, 1);
}

static void flowie_mqtt_test_publish_completion(flowie_mqtt_client_t *client, int status,
                                                const flowie_mqtt_control_packet_view_t *response,
                                                void *user_data) {
  flowie_mqtt_test_state_t *state = (flowie_mqtt_test_state_t *)user_data;
  int index = atomic_fetch_add_explicit(&state->publish_completions, 1, memory_order_relaxed);
  flowie_mqtt_packet_type_t expected =
      index == 0 ? FLOWIE_MQTT_PACKET_PUBACK : FLOWIE_MQTT_PACKET_PUBCOMP;
  (void)client;
  if (index > 1) status = TURBO_EPROTO;
  flowie_mqtt_test_record_completion(state, status, response, expected, 1);
}

static void
flowie_mqtt_test_unsubscribe_completion(flowie_mqtt_client_t *client, int status,
                                        const flowie_mqtt_control_packet_view_t *response,
                                        void *user_data) {
  (void)client;
  flowie_mqtt_test_record_completion((flowie_mqtt_test_state_t *)user_data, status, response,
                                     FLOWIE_MQTT_PACKET_UNSUBACK, 1);
}

static void flowie_mqtt_test_ping_completion(flowie_mqtt_client_t *client, int status,
                                             const flowie_mqtt_control_packet_view_t *response,
                                             void *user_data) {
  (void)client;
  flowie_mqtt_test_record_completion((flowie_mqtt_test_state_t *)user_data, status, response,
                                     (flowie_mqtt_packet_type_t)0, 0);
}

static void
flowie_mqtt_test_disconnect_completion(flowie_mqtt_client_t *client, int status,
                                       const flowie_mqtt_control_packet_view_t *response,
                                       void *user_data) {
  (void)client;
  flowie_mqtt_test_record_completion((flowie_mqtt_test_state_t *)user_data, status, response,
                                     (flowie_mqtt_packet_type_t)0, 0);
}

static void flowie_mqtt_test_error(flowie_mqtt_client_t *client, int status, void *user_data) {
  flowie_mqtt_test_state_t *state = (flowie_mqtt_test_state_t *)user_data;
  (void)client;
  atomic_store_explicit(&state->background_error, status, memory_order_release);
}

static void flowie_mqtt_test_count_completion(flowie_mqtt_client_t *client, int status,
                                              const flowie_mqtt_control_packet_view_t *response,
                                              void *user_data) {
  int *count = (int *)user_data;
  (void)client;
  (void)status;
  (void)response;
  ++*count;
}

static void flowie_mqtt_test_shutdown_completion(flowie_mqtt_client_t *client, int status,
                                                 const flowie_mqtt_control_packet_view_t *response,
                                                 void *user_data) {
  flowie_mqtt_test_shutdown_t *shutdown = (flowie_mqtt_test_shutdown_t *)user_data;
  (void)client;
  (void)response;
  if (shutdown->count < (int)(sizeof(shutdown->statuses) / sizeof(shutdown->statuses[0])))
    shutdown->statuses[shutdown->count] = status;
  ++shutdown->count;
}

static void
flowie_mqtt_reentrant_connect_completion(flowie_mqtt_client_t *client, int status,
                                         const flowie_mqtt_control_packet_view_t *response,
                                         void *user_data) {
  flowie_mqtt_reentrant_shutdown_t *state = (flowie_mqtt_reentrant_shutdown_t *)user_data;
  int enqueue_status = TURBO_EPROTO;
  if (status == TURBO_OK && response && response->type == FLOWIE_MQTT_PACKET_CONNACK &&
      response->reason_code == 0u)
    enqueue_status = flowie_mqtt_client_ping(client);
  atomic_store_explicit(&state->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->enqueue_status, enqueue_status, memory_order_relaxed);
  atomic_store_explicit(&state->connect_done, 1, memory_order_release);
}

static void flowie_mqtt_reentrant_ping_completion(
    flowie_mqtt_client_t *client, int status, const flowie_mqtt_control_packet_view_t *response,
    void *user_data) {
  flowie_mqtt_reentrant_shutdown_t *state = (flowie_mqtt_reentrant_shutdown_t *)user_data;
  (void)client;
  (void)response;
  atomic_store_explicit(&state->ping_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->ping_count, 1, memory_order_release);
}

static void flowie_mqtt_qos2_wake_connect_completion(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user_data) {
  flowie_mqtt_qos2_wake_state_t *state = (flowie_mqtt_qos2_wake_state_t *)user_data;
  (void)client;
  if (status == TURBO_OK &&
      (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK || response->reason_code != 0u))
    status = TURBO_EPROTO;
  atomic_store_explicit(&state->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->connect_done, 1, memory_order_release);
}

static void flowie_mqtt_qos2_wake_publish_completion(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user_data) {
  flowie_mqtt_qos2_wake_state_t *state = (flowie_mqtt_qos2_wake_state_t *)user_data;
  (void)client;
  if (status == TURBO_OK &&
      (!response || response->type != FLOWIE_MQTT_PACKET_PUBCOMP || response->reason_code >= 0x80u))
    status = TURBO_EPROTO;
  atomic_store_explicit(&state->publish_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->publish_count, 1, memory_order_release);
}

static void flowie_mqtt_qos2_wake_disconnect_completion(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user_data) {
  flowie_mqtt_qos2_wake_state_t *state = (flowie_mqtt_qos2_wake_state_t *)user_data;
  (void)client;
  if (response) status = TURBO_EPROTO;
  atomic_store_explicit(&state->disconnect_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->disconnect_done, 1, memory_order_release);
}

static int flowie_mqtt_qos2_wake_on_message(flowie_mqtt_client_t *client,
                                             const flowie_mqtt_publish_view_t *publish,
                                             void *user_data) {
  flowie_mqtt_qos2_wake_state_t *state = (flowie_mqtt_qos2_wake_state_t *)user_data;
  int inbound_count;
  (void)client;
  if (!publish || publish->qos != 0u) return TURBO_EPROTO;
  inbound_count = atomic_fetch_add_explicit(&state->inbound_count, 1, memory_order_acq_rel) + 1;
  if ((inbound_count & 1) != 0)
    atomic_store_explicit(&state->pulse_count, (inbound_count + 1) / 2, memory_order_release);
  return TURBO_OK;
}

static void
flowie_mqtt_test_error_connect_completion(flowie_mqtt_client_t *client, int status,
                                          const flowie_mqtt_control_packet_view_t *response,
                                          void *user_data) {
  flowie_mqtt_test_error_state_t *state = (flowie_mqtt_test_error_state_t *)user_data;
  (void)client;
  if (status == TURBO_OK &&
      (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK || response->reason_code != 0u))
    status = TURBO_EPROTO;
  atomic_store_explicit(&state->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->connect_done, 1, memory_order_release);
}

static void flowie_mqtt_test_background_error(flowie_mqtt_client_t *client, int status,
                                              void *user_data) {
  flowie_mqtt_test_error_state_t *state = (flowie_mqtt_test_error_state_t *)user_data;
  (void)client;
  atomic_store_explicit(&state->error_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->error_count, 1, memory_order_release);
}

static void flowie_mqtt_limits_connect_completion(flowie_mqtt_client_t *client, int status,
                                                  const flowie_mqtt_control_packet_view_t *response,
                                                  void *user_data) {
  flowie_mqtt_limits_state_t *state = (flowie_mqtt_limits_state_t *)user_data;
  (void)client;
  if (status == TURBO_OK &&
      (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK || response->reason_code != 0u))
    status = TURBO_EPROTO;
  atomic_store_explicit(&state->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->connect_done, 1, memory_order_release);
}

static void flowie_mqtt_limits_publish_completion(flowie_mqtt_client_t *client, int status,
                                                  const flowie_mqtt_control_packet_view_t *response,
                                                  void *user_data) {
  flowie_mqtt_limits_state_t *state = (flowie_mqtt_limits_state_t *)user_data;
  int index = atomic_fetch_add_explicit(&state->publish_count, 1, memory_order_acq_rel);
  (void)client;
  (void)response;
  if (index >= 0 && index < (int)(sizeof(state->publish_status) / sizeof(state->publish_status[0])))
    state->publish_status[index] = status;
}

static void
flowie_mqtt_limits_disconnect_completion(flowie_mqtt_client_t *client, int status,
                                         const flowie_mqtt_control_packet_view_t *response,
                                         void *user_data) {
  flowie_mqtt_limits_state_t *state = (flowie_mqtt_limits_state_t *)user_data;
  (void)client;
  (void)response;
  atomic_store_explicit(&state->disconnect_done, status == TURBO_OK ? 1 : -1, memory_order_release);
}

static void flowie_mqtt_limits_broker_handler(coro_socket_t *socket, void *arg) {
  static const uint8_t properties[] = {FLOWIE_MQTT_PROPERTY_RECEIVE_MAXIMUM,
                                       0x00u,
                                       0x01u,
                                       FLOWIE_MQTT_PROPERTY_TOPIC_ALIAS_MAXIMUM,
                                       0x00u,
                                       0x01u,
                                       FLOWIE_MQTT_PROPERTY_MAXIMUM_QOS,
                                       0x01u,
                                       FLOWIE_MQTT_PROPERTY_RETAIN_AVAILABLE,
                                       0x00u,
                                       FLOWIE_MQTT_PROPERTY_MAXIMUM_PACKET_SIZE,
                                       0x00u,
                                       0x00u,
                                       0x00u,
                                       0x14u};
  flowie_mqtt_limits_state_t *state = (flowie_mqtt_limits_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_connack_properties(
        socket, (flowie_mqtt_span_t){properties, sizeof(properties)});
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5, FLOWIE_MQTT_PACKET_DISCONNECT,
                                      &packet);
  state->server_status = rc;
  state->server_done = 1;
}

static void flowie_mqtt_auth_connect_completion(flowie_mqtt_client_t *client, int status,
                                                const flowie_mqtt_control_packet_view_t *response,
                                                void *user_data) {
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)user_data;
  (void)client;
  if (status == TURBO_OK &&
      (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK || response->reason_code != 0u ||
       !flowie_mqtt_test_auth_properties_match(&response->properties, "scram", "server-final")))
    status = TURBO_EPROTO;
  atomic_store_explicit(&state->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->connect_done, 1, memory_order_release);
}

static void
flowie_mqtt_auth_plain_connect_completion(flowie_mqtt_client_t *client, int status,
                                          const flowie_mqtt_control_packet_view_t *response,
                                          void *user_data) {
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)user_data;
  (void)client;
  if (status == TURBO_OK &&
      (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK || response->reason_code != 0u))
    status = TURBO_EPROTO;
  atomic_store_explicit(&state->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->connect_done, 1, memory_order_release);
}

static int flowie_mqtt_auth_challenge(flowie_mqtt_client_t *client,
                                      const flowie_mqtt_control_packet_view_t *challenge,
                                      flowie_mqtt_client_auth_response_t *response,
                                      void *user_data) {
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)user_data;
  int index = atomic_fetch_add_explicit(&state->challenge_count, 1, memory_order_acq_rel);
  const char *expected_data = index == 0 ? "server-first" : "server-reauth";
  (void)client;
  if (!challenge || !response || index < 0 || index > 1 || challenge->reason_code != 0x18u ||
      !flowie_mqtt_test_auth_properties_match(&challenge->properties, "scram", expected_data))
    return TURBO_EPROTO;
  if (state->fail_challenge && index == state->fail_challenge_index) return TURBO_EIO;
  response->properties =
      index == 0 ? (flowie_mqtt_span_t){state->initial_response, state->initial_response_size}
                 : (flowie_mqtt_span_t){state->reauth_response, state->reauth_response_size};
  return TURBO_OK;
}

static void flowie_mqtt_auth_completion(flowie_mqtt_client_t *client, int status,
                                        const flowie_mqtt_control_packet_view_t *response,
                                        void *user_data) {
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)user_data;
  (void)client;
  if (status == TURBO_OK &&
      (!response || response->type != FLOWIE_MQTT_PACKET_AUTH || response->reason_code != 0u ||
       !flowie_mqtt_test_auth_properties_match(&response->properties, "scram", "reauth-final")))
    status = TURBO_EPROTO;
  atomic_store_explicit(&state->auth_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->auth_done, 1, memory_order_release);
}

static void
flowie_mqtt_auth_disconnect_completion(flowie_mqtt_client_t *client, int status,
                                       const flowie_mqtt_control_packet_view_t *response,
                                       void *user_data) {
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)user_data;
  (void)client;
  (void)response;
  atomic_store_explicit(&state->disconnect_done, status == TURBO_OK ? 1 : -1, memory_order_release);
}

static int flowie_mqtt_auth_expect(flowie_mqtt_test_broker_stream_t *stream, uint8_t reason_code,
                                   const char *data) {
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  int rc =
      flowie_mqtt_test_expect_type(stream, FLOWIE_MQTT_VERSION_5, FLOWIE_MQTT_PACKET_AUTH, &packet);
  if (rc != TURBO_OK) return rc;
  rc = flowie_mqtt_control_packet_parse(&packet, &control);
  if (rc != FLOWIE_MQTT_PARSE_OK || control.reason_code != reason_code ||
      !flowie_mqtt_test_auth_properties_match(&control.properties, "scram", data))
    return TURBO_EPROTO;
  return TURBO_OK;
}

static void flowie_mqtt_auth_broker_handler(coro_socket_t *socket, void *arg) {
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  uint8_t properties[64];
  size_t properties_size = 0u;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK) {
    rc = flowie_mqtt_connect_parse(&packet, &connect);
    if (rc != FLOWIE_MQTT_PARSE_OK || connect.version != FLOWIE_MQTT_VERSION_5 ||
        !flowie_mqtt_test_auth_properties_match(&connect.properties, "scram", "client-first"))
      rc = TURBO_EPROTO;
  }
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_auth_properties_encode("scram", "server-first", properties,
                                                 sizeof(properties), &properties_size);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control_properties(
        socket, FLOWIE_MQTT_PACKET_AUTH, 0x18u, (flowie_mqtt_span_t){properties, properties_size});
  if (rc == TURBO_OK) rc = flowie_mqtt_auth_expect(&stream, 0x18u, "client-response");
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_auth_properties_encode("scram", "server-final", properties,
                                                 sizeof(properties), &properties_size);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_connack_properties(
        socket, (flowie_mqtt_span_t){properties, properties_size});
  if (rc == TURBO_OK) rc = flowie_mqtt_auth_expect(&stream, 0x19u, "reauth-start");
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_auth_properties_encode("scram", "server-reauth", properties,
                                                 sizeof(properties), &properties_size);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control_properties(
        socket, FLOWIE_MQTT_PACKET_AUTH, 0x18u, (flowie_mqtt_span_t){properties, properties_size});
  if (rc == TURBO_OK) rc = flowie_mqtt_auth_expect(&stream, 0x18u, "reauth-response");
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_auth_properties_encode("scram", "reauth-final", properties,
                                                 sizeof(properties), &properties_size);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control_properties(
        socket, FLOWIE_MQTT_PACKET_AUTH, 0u, (flowie_mqtt_span_t){properties, properties_size});
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5, FLOWIE_MQTT_PACKET_DISCONNECT,
                                      &packet);
  state->server_status = rc;
  state->server_done = 1;
}

static void flowie_mqtt_auth_mismatch_broker_handler(coro_socket_t *socket, void *arg) {
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  uint8_t properties[64];
  size_t properties_size = 0u;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_auth_properties_encode("different", "server-first", properties,
                                                 sizeof(properties), &properties_size);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control_properties(
        socket, FLOWIE_MQTT_PACKET_AUTH, 0x18u, (flowie_mqtt_span_t){properties, properties_size});
  state->server_status = rc;
  state->server_done = 1;
}

static void flowie_mqtt_auth_missing_method_broker_handler(coro_socket_t *socket, void *arg) {
  static const uint8_t properties[] = {FLOWIE_MQTT_PROPERTY_AUTHENTICATION_DATA,
                                       0x00u,
                                       0x0cu,
                                       's',
                                       'e',
                                       'r',
                                       'v',
                                       'e',
                                       'r',
                                       '-',
                                       'f',
                                       'i',
                                       'r',
                                       's',
                                       't'};
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control_properties(
        socket, FLOWIE_MQTT_PACKET_AUTH, 0x18u,
        (flowie_mqtt_span_t){properties, sizeof(properties)});
  state->server_status = rc;
  state->server_done = 1;
}

static void flowie_mqtt_auth_oversized_broker_handler(coro_socket_t *socket, void *arg) {
  static const char oversized_data[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  uint8_t properties[128];
  size_t properties_size = 0u;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_auth_properties_encode("scram", oversized_data, properties,
                                                 sizeof(properties), &properties_size);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control_properties(
        socket, FLOWIE_MQTT_PACKET_AUTH, 0x18u,
        (flowie_mqtt_span_t){properties, properties_size});
  state->server_status = rc;
  state->server_done = 1;
}

static void flowie_mqtt_auth_v3_broker_handler(coro_socket_t *socket, void *arg) {
  flowie_mqtt_auth_state_t *state = (flowie_mqtt_auth_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_3_1_1,
                                       FLOWIE_MQTT_PACKET_CONNACK, 0u, 0u, (flowie_mqtt_span_t){0});
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_3_1_1,
                                      FLOWIE_MQTT_PACKET_DISCONNECT, &packet);
  state->server_status = rc;
  state->server_done = 1;
}

static void flowie_mqtt_test_closing_broker_handler(coro_socket_t *socket, void *arg) {
  flowie_mqtt_test_error_state_t *state = (flowie_mqtt_test_error_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK && (flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK ||
                         connect.version != FLOWIE_MQTT_VERSION_5))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5, FLOWIE_MQTT_PACKET_CONNACK,
                                       0u, 0u, (flowie_mqtt_span_t){0});
  if (rc == TURBO_OK) coro_sleep(coro_context_current(), FLOWIE_MQTT_CLIENT_TEST_CLOSE_GRACE_MS);
  state->server_status = rc;
  state->server_done = 1;
}

static void flowie_mqtt_server_busy_then_ready_handler(coro_socket_t *socket, void *arg) {
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  int attempt = atomic_fetch_add_explicit(&state->broker_connects, 1, memory_order_acq_rel) + 1;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK &&
      (flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK ||
       connect.version != FLOWIE_MQTT_VERSION_5))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5,
                                       FLOWIE_MQTT_PACKET_CONNACK, 0u,
                                       attempt == 1 ? 0x89u : 0u,
                                       (flowie_mqtt_span_t){0});
  if (rc == TURBO_OK && attempt != 1) {
    packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                      FLOWIE_MQTT_PACKET_DISCONNECT, &packet);
    if (rc == TURBO_EOF || rc == TURBO_ECONNRESET) rc = TURBO_OK;
  }
  if (rc != TURBO_OK)
    atomic_store_explicit(&state->server_status, rc, memory_order_release);
}

static void flowie_mqtt_expired_token_then_ready_handler(coro_socket_t *socket, void *arg) {
  static const uint8_t initial_token[] = "token-1";
  static const uint8_t refreshed_token[] = "token-2";
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  int attempt = atomic_fetch_add_explicit(&state->broker_connects, 1, memory_order_acq_rel) + 1;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK &&
      (flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK ||
       connect.version != FLOWIE_MQTT_VERSION_5 || connect.password.size == 0u))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK && attempt == 1 &&
      (connect.password.size != sizeof(initial_token) - 1u ||
       memcmp(connect.password.data, initial_token, sizeof(initial_token) - 1u) != 0))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK && attempt != 1) {
    if (connect.password.size != sizeof(refreshed_token) - 1u ||
        memcmp(connect.password.data, refreshed_token, sizeof(refreshed_token) - 1u) != 0)
      rc = TURBO_EPROTO;
    else
      atomic_store_explicit(&state->refreshed_token_seen, 1, memory_order_release);
  }
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5,
                                       FLOWIE_MQTT_PACKET_CONNACK, 0u,
                                       attempt == 1 ? 0x87u : 0u,
                                       (flowie_mqtt_span_t){0});
  if (rc == TURBO_OK && attempt != 1) {
    packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                      FLOWIE_MQTT_PACKET_DISCONNECT, &packet);
    if (rc == TURBO_EOF || rc == TURBO_ECONNRESET) rc = TURBO_OK;
  }
  if (rc != TURBO_OK)
    atomic_store_explicit(&state->server_status, rc, memory_order_release);
}

static void flowie_mqtt_disconnect_then_reconnect_handler(coro_socket_t *socket, void *arg) {
  static const uint8_t initial_token[] = "token-1";
  static const uint8_t refreshed_token[] = "token-2";
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  int attempt = atomic_fetch_add_explicit(&state->broker_connects, 1, memory_order_acq_rel) + 1;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK &&
      (flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK ||
       connect.version != FLOWIE_MQTT_VERSION_5))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK && attempt == 1 &&
      (connect.password.size != sizeof(initial_token) - 1u ||
       memcmp(connect.password.data, initial_token, sizeof(initial_token) - 1u) != 0))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK && attempt != 1) {
    if (state->disconnect_reason != 0x87u ||
        connect.password.size != sizeof(refreshed_token) - 1u ||
        memcmp(connect.password.data, refreshed_token, sizeof(refreshed_token) - 1u) != 0)
      rc = TURBO_EPROTO;
    else
      atomic_store_explicit(&state->refreshed_token_seen, 1, memory_order_release);
  }
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5,
                                       FLOWIE_MQTT_PACKET_CONNACK, 0u, 0u,
                                       (flowie_mqtt_span_t){0});
  if (rc == TURBO_OK && attempt == 1)
    rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5,
                                       FLOWIE_MQTT_PACKET_DISCONNECT, 0u,
                                       state->disconnect_reason, (flowie_mqtt_span_t){0});
  if (rc == TURBO_OK && attempt != 1) {
    packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                      FLOWIE_MQTT_PACKET_DISCONNECT, &packet);
    if (rc == TURBO_EOF || rc == TURBO_ECONNRESET) rc = TURBO_OK;
  }
  if (rc != TURBO_OK)
    atomic_store_explicit(&state->server_status, rc, memory_order_release);
}

static void flowie_mqtt_ping_close_then_ready_handler(coro_socket_t *socket, void *arg) {
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)arg;
  flowie_mqtt_test_broker_stream_t stream = {0};
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  int attempt = atomic_fetch_add_explicit(&state->broker_connects, 1, memory_order_acq_rel) + 1;
  int rc;
  stream.socket = socket;
  coro_socket_set_timeout(socket, FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS);
  rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_UNSPECIFIED,
                                    FLOWIE_MQTT_PACKET_CONNECT, &packet);
  if (rc == TURBO_OK &&
      (flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK ||
       connect.version != FLOWIE_MQTT_VERSION_5))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK)
    rc = flowie_mqtt_test_send_control(socket, FLOWIE_MQTT_VERSION_5,
                                       FLOWIE_MQTT_PACKET_CONNACK, 0u, 0u,
                                       (flowie_mqtt_span_t){0});
  if (rc == TURBO_OK && attempt == 1) {
    packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                      FLOWIE_MQTT_PACKET_PINGREQ, &packet);
  } else if (rc == TURBO_OK) {
    packet = (flowie_mqtt_packet_view_t)FLOWIE_MQTT_PACKET_VIEW_INIT;
    rc = flowie_mqtt_test_expect_type(&stream, FLOWIE_MQTT_VERSION_5,
                                      FLOWIE_MQTT_PACKET_DISCONNECT, &packet);
    if (rc == TURBO_EOF || rc == TURBO_ECONNRESET) rc = TURBO_OK;
  }
  if (rc != TURBO_OK)
    atomic_store_explicit(&state->server_status, rc, memory_order_release);
}

static int flowie_mqtt_refresh_expired_connect(
    flowie_mqtt_client_t *client, uint8_t reason_code,
    const flowie_mqtt_connect_packet_t *current, flowie_mqtt_connect_packet_t *refreshed,
    void *user_data) {
  static const uint8_t initial_token[] = "token-1";
  static const uint8_t refreshed_token[] = "token-2";
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)user_data;
  (void)client;
  if (reason_code != 0x87u || !current || !refreshed || !current->has_password ||
      current->password.size != sizeof(initial_token) - 1u ||
      memcmp(current->password.data, initial_token, sizeof(initial_token) - 1u) != 0)
    return TURBO_EPROTO;
  *refreshed = *current;
  refreshed->has_password = 1u;
  refreshed->password =
      (flowie_mqtt_span_t){refreshed_token, sizeof(refreshed_token) - 1u};
  atomic_fetch_add_explicit(&state->refresh_calls, 1, memory_order_release);
  return TURBO_OK;
}

static int flowie_mqtt_refresh_with_conflicting_version(
    flowie_mqtt_client_t *client, uint8_t reason_code,
    const flowie_mqtt_connect_packet_t *current, flowie_mqtt_connect_packet_t *refreshed,
    void *user_data) {
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)user_data;
  (void)client;
  if (reason_code != 0x87u || !current || !refreshed) return TURBO_EPROTO;
  *refreshed = *current;
  refreshed->version = FLOWIE_MQTT_VERSION_3_1_1;
  atomic_fetch_add_explicit(&state->refresh_calls, 1, memory_order_release);
  return TURBO_OK;
}

static void flowie_mqtt_resilience_connect_completion(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user_data) {
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)user_data;
  (void)client;
  if (status == TURBO_OK && (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK))
    status = TURBO_EPROTO;
  atomic_store_explicit(&state->initial_reason,
                        response ? (int)response->reason_code : -1, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->initial_completions, 1, memory_order_release);
  if (status != TURBO_OK)
    atomic_store_explicit(&state->server_status, status, memory_order_release);
}

static void flowie_mqtt_resilience_reconnect_completion(
    flowie_mqtt_client_t *client, uint32_t attempt, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user_data) {
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)user_data;
  (void)client;
  atomic_store_explicit(&state->reconnect_attempt, (int)attempt, memory_order_relaxed);
  atomic_store_explicit(&state->reconnect_status, status, memory_order_relaxed);
  atomic_store_explicit(&state->reconnect_reason,
                        response ? (int)response->reason_code : -1, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->reconnect_completions, 1, memory_order_release);
}

static void flowie_mqtt_resilience_error(flowie_mqtt_client_t *client, int status,
                                         void *user_data) {
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)user_data;
  (void)client;
  atomic_store_explicit(&state->background_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->background_errors, 1, memory_order_release);
}

static void flowie_mqtt_resilience_connect_and_ping(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user_data) {
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)user_data;
  int submit_status = TURBO_EPROTO;
  if (status == TURBO_OK && response && response->type == FLOWIE_MQTT_PACKET_CONNACK &&
      response->reason_code == 0u)
    submit_status = flowie_mqtt_client_ping(client);
  atomic_store_explicit(&state->initial_reason,
                        response ? (int)response->reason_code : -1, memory_order_relaxed);
  atomic_store_explicit(&state->ping_submit_status, submit_status, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->initial_completions, 1, memory_order_release);
}

static void flowie_mqtt_resilience_ping_completion(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user_data) {
  flowie_mqtt_resilience_state_t *state = (flowie_mqtt_resilience_state_t *)user_data;
  (void)client;
  if (response) status = TURBO_EPROTO;
  atomic_store_explicit(&state->ping_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&state->ping_completions, 1, memory_order_release);
}

static int flowie_mqtt_test_run_callbacks(flowie_mqtt_version_t version, int select_version,
                                          flowie_mqtt_version_t packet_version,
                                          flowie_mqtt_test_state_t *state) {
  enum { FLOWIE_MQTT_MANAGED_COMMAND_COUNT = 7 };
  uint8_t client_id[] = "flowie-callback-client";
  uint8_t filter_one[] = "client/topic/one";
  uint8_t filter_two[] = "client/topic/two";
  uint8_t inbound_filter[] = "server/topic/+";
  uint8_t exact_filter[] = "server/topic/two";
  uint8_t payload_one[] = "from-client-one";
  uint8_t payload_two[] = "from-client-two";
  flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_client_topic_handler_t topic_handlers[2] = {{0}};
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_subscription_t subscriptions[2] = {{0}};
  flowie_mqtt_subscribe_packet_t subscribe = FLOWIE_MQTT_SUBSCRIBE_PACKET_INIT;
  flowie_mqtt_client_publish_topic_t publish_topics[2] = {{0}};
  flowie_mqtt_client_publish_topic_vec_t publish = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
  flowie_mqtt_unsubscribe_packet_t unsubscribe = FLOWIE_MQTT_UNSUBSCRIBE_PACKET_INIT;
  flowie_mqtt_span_t filters[2] = {{filter_one, sizeof(filter_one) - 1u},
                                   {filter_two, sizeof(filter_two) - 1u}};
  coro_socket_t *server = NULL;
  unsigned short port = flowie_test_port();
  uint64_t deadline;
  int rc = TURBO_OK;

  if (port == 0u) return TURBO_EIO;
  memset(state, 0, sizeof(*state));
  state->version = version;
  state->expect_disconnect = 0;
  state->server_rc = TURBO_EBUSY;
  atomic_init(&state->completions, 0);
  atomic_init(&state->publish_completions, 0);
  atomic_init(&state->completion_error, TURBO_OK);
  atomic_init(&state->background_error, TURBO_OK);
  state->context = coro_context_create(NULL);
  if (!state->context) return TURBO_ENOMEM;
  server = coro_socket_create_tcpv4(state->context);
  if (!server) {
    rc = TURBO_ENOMEM;
    goto done;
  }
  config.host = "127.0.0.1";
  config.port = port;
  config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
  config.command_queue_capacity = 8u;
  topic_handlers[0].filter = (flowie_mqtt_span_t){inbound_filter, sizeof(inbound_filter) - 1u};
  topic_handlers[0].on_message = flowie_mqtt_test_on_publish;
  topic_handlers[1].filter = (flowie_mqtt_span_t){exact_filter, sizeof(exact_filter) - 1u};
  topic_handlers[1].on_message = flowie_mqtt_test_on_secondary_match;
  config.topic_handlers = (flowie_mqtt_client_topic_handler_map_t){topic_handlers, 2u};
  config.on_connect = flowie_mqtt_test_connect_completion;
  config.on_publish = flowie_mqtt_test_publish_completion;
  config.on_subscribe = flowie_mqtt_test_subscribe_completion;
  config.on_unsubscribe = flowie_mqtt_test_unsubscribe_completion;
  config.on_ping = flowie_mqtt_test_ping_completion;
  config.on_disconnect = flowie_mqtt_test_disconnect_completion;
  config.on_error = flowie_mqtt_test_error;
  config.user_data = state;
  rc = coro_socket_listen_on(server, config.host, port, flowie_mqtt_test_broker_handler, state);
  if (rc != TURBO_OK) goto done;
  rc = flowie_mqtt_client_create(&config, &state->client);
  if (rc != TURBO_OK) goto done;
  if (select_version) {
    rc = flowie_mqtt_client_set_version(state->client, version);
    if (rc != TURBO_OK) goto done;
  }
  connect.version = packet_version;
  connect.clean_start = 1u;
  connect.keep_alive = 30u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  subscriptions[0].filter = filters[0];
  subscriptions[0].qos = 1u;
  subscriptions[1].filter = filters[1];
  subscriptions[1].qos = 1u;
  subscribe.version = packet_version;
  subscribe.subscriptions = subscriptions;
  subscribe.subscription_count = 2u;
  publish.version = packet_version;
  publish.data = publish_topics;
  publish.count = 2u;
  publish_topics[0].qos = 1u;
  publish_topics[0].topic = filters[0];
  publish_topics[0].payload = (flowie_mqtt_span_t){payload_one, sizeof(payload_one) - 1u};
  publish_topics[1].qos = 2u;
  publish_topics[1].topic = filters[1];
  publish_topics[1].payload = (flowie_mqtt_span_t){payload_two, sizeof(payload_two) - 1u};
  unsubscribe.version = packet_version;
  unsubscribe.filters = filters;
  unsubscribe.filter_count = 2u;

  rc = flowie_mqtt_client_connect(state->client, &connect);
  if (rc != TURBO_OK) goto done;
  rc = flowie_mqtt_client_subscribe(state->client, &subscribe);
  if (rc != TURBO_OK) goto done;
  rc = flowie_mqtt_client_publish(state->client, &publish);
  if (rc != TURBO_OK) goto done;
  rc = flowie_mqtt_client_unsubscribe(state->client, &unsubscribe);
  if (rc != TURBO_OK) goto done;
  rc = flowie_mqtt_client_ping(state->client);
  if (rc != TURBO_OK) goto done;
  rc = flowie_mqtt_client_disconnect(state->client, 0u, (flowie_mqtt_span_t){0});
  if (rc != TURBO_OK) goto done;

  memset(client_id, 0, sizeof(client_id));
  memset(filter_one, 0, sizeof(filter_one));
  memset(filter_two, 0, sizeof(filter_two));
  memset(inbound_filter, 0, sizeof(inbound_filter));
  memset(exact_filter, 0, sizeof(exact_filter));
  memset(payload_one, 0, sizeof(payload_one));
  memset(payload_two, 0, sizeof(payload_two));
  deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS * 2u;
  while ((!state->server_done || atomic_load_explicit(&state->completions, memory_order_acquire) !=
                                     FLOWIE_MQTT_MANAGED_COMMAND_COUNT) &&
         turbo_monotonic_ms() < deadline)
    (void)coro_context_run(state->context, TURBO_RUN_ONCE);
  if (!state->server_done || atomic_load_explicit(&state->completions, memory_order_acquire) !=
                                 FLOWIE_MQTT_MANAGED_COMMAND_COUNT)
    rc = TURBO_ETIMEDOUT;
  else if (atomic_load_explicit(&state->completion_error, memory_order_acquire) != TURBO_OK)
    rc = atomic_load_explicit(&state->completion_error, memory_order_acquire);
  else if (atomic_load_explicit(&state->background_error, memory_order_acquire) != TURBO_OK)
    rc = atomic_load_explicit(&state->background_error, memory_order_acquire);
  else rc = state->server_rc;
  if (rc == TURBO_EOF) rc = TURBO_OK;

done:
  if (server) coro_socket_destroy(server);
  flowie_mqtt_client_destroy(state->client);
  coro_context_destroy(state->context);
  state->client = NULL;
  state->context = NULL;
  return rc;
}

spec("flowie mqtt callback client") {
  it("validates configuration and starts disconnected") {
    static const uint8_t duplicate_filter[] = "duplicate/#";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_client_topic_handler_t duplicate_handlers[2] = {{0}};
    flowie_mqtt_client_t *client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_EINVAL);
    check_null(client);
    config.host = "127.0.0.1";
    config.stream_recv_buffer_bytes = 128u * 1024u;
    config.socket_recv_buffer_bytes = 1024u * 1024u;
    config.socket_send_buffer_bytes = 512u * 1024u;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    check_not_null(client);
    check_false(flowie_mqtt_client_is_connected(client));
    check_equal(flowie_mqtt_client_ping(client), TURBO_ENOTSUP);
    flowie_mqtt_client_destroy(client);
    client = NULL;
    config.size -= 1u;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_EINVAL);
    check_null(client);
    config.size = sizeof(config);
    config.socket_recv_buffer_bytes = (size_t)INT_MAX + 1u;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_ERANGE);
    check_null(client);
    config.socket_recv_buffer_bytes = 0u;
    config.stream_recv_buffer_bytes = FLOWIE_MQTT_CLIENT_MIN_STREAM_RECV_BUFFER_SIZE - 1u;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_ERANGE);
    check_null(client);
    config.stream_recv_buffer_bytes = 0u;
    resilience.size -= 1u;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_EINVAL);
    check_null(client);
    resilience.size = sizeof(resilience);
    resilience.initial_delay_ms = 100u;
    resilience.max_delay_ms = 10u;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_EINVAL);
    check_null(client);
    resilience = (flowie_mqtt_client_resilience_config_t)
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_OK);
    check_not_null(client);
    flowie_mqtt_client_destroy(client);
    client = NULL;
    duplicate_handlers[0].filter =
        (flowie_mqtt_span_t){duplicate_filter, sizeof(duplicate_filter) - 1u};
    duplicate_handlers[0].on_message = flowie_mqtt_test_on_publish;
    duplicate_handlers[1] = duplicate_handlers[0];
    config.topic_handlers = (flowie_mqtt_client_topic_handler_map_t){duplicate_handlers, 2u};
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_EINVAL);
    check_null(client);
  }

  it("validates protocol selection and locks it after command admission") {
    static const uint8_t client_id[] = "flowie-version-boundary";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_client_t *client = NULL;
    unsigned short port = flowie_test_port();
    check_not_equal(port, 0u);
    check_equal(flowie_mqtt_client_set_version(NULL, FLOWIE_MQTT_VERSION_5), TURBO_EINVAL);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = 50u;
    config.on_connect = flowie_mqtt_test_ignore_completion;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    check_equal(flowie_mqtt_client_set_version(client, FLOWIE_MQTT_VERSION_UNSPECIFIED),
                TURBO_EINVAL);
    check_equal(flowie_mqtt_client_set_version(client, (flowie_mqtt_version_t)2),
                TURBO_EINVAL);
    check_equal(flowie_mqtt_client_set_version(client, (flowie_mqtt_version_t)6),
                TURBO_EINVAL);
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    connect.version = FLOWIE_MQTT_VERSION_3_1_1;
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_EPROTO);
    check_equal(flowie_mqtt_client_set_version(client, FLOWIE_MQTT_VERSION_3_1_1), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_EPROTO);
    connect.version = FLOWIE_MQTT_VERSION_UNSPECIFIED;
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    check_equal(flowie_mqtt_client_set_version(client, FLOWIE_MQTT_VERSION_3_1_1),
                TURBO_EALREADY);
    check_equal(flowie_mqtt_client_set_version(client, FLOWIE_MQTT_VERSION_5), TURBO_EALREADY);
    flowie_mqtt_client_destroy(client);
  }

  it("accepts verified TLS client identity and rejects incomplete credentials") {
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_t *client = NULL;

    config.host = "localhost";
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
    config.port = FLOWIE_MQTT_CLIENT_DEFAULT_TLS_PORT;
    config.tls.ca_file = "ca.pem";
    config.tls.cert_file = "client.pem";
    config.tls.key_file = "client-key.pem";
    config.tls.key_password = "secret";
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    check_not_null(client);
    flowie_mqtt_client_destroy(client);

    client = NULL;
    config.tls.key_file = NULL;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_EINVAL);
    check_null(client);
    config.tls.key_file = "client-key.pem";
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TCP;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_EINVAL);
    check_null(client);
  }

  it("presents its client certificate to an mTLS MQTT server") {
    static const uint8_t connack[] = {0x20u, 0x03u, 0x00u, 0x00u, 0x00u};
    uint8_t client_id[] = "flowie-mtls-client";
    char ca_file[512] = {0};
    char cert_file[512] = {0};
    char key_file[512] = {0};
    flow_mtls_test_server_t server;
    flowie_mqtt_mtls_state_t state;
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_client_t *client = NULL;
    uint64_t deadline;

    atomic_init(&state.done, 0);
    atomic_init(&state.status, TURBO_EBUSY);
    check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    check_equal(flow_mtls_test_server_start(&server, connack, sizeof(connack)), 0);
    config.host = "localhost";
    config.port = server.port;
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.tls.ca_file = ca_file;
    config.tls.cert_file = cert_file;
    config.tls.key_file = key_file;
    config.on_connect = flowie_mqtt_mtls_connect_completion;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      turbo_sleep_ms(1u);
    flow_mtls_test_server_join(&server);
    check_true(atomic_load_explicit(&state.done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.status, memory_order_relaxed), TURBO_OK);
    check_equal(server.status, 0);
    check_true(server.peer_verified);
    flowie_mqtt_client_destroy(client);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
    tls_test_remove_file(ca_file);
  }

  it("is rejected by an mTLS MQTT server when no client certificate is configured") {
    static const uint8_t connack[] = {0x20u, 0x03u, 0x00u, 0x00u, 0x00u};
    static const uint8_t client_id[] = "flowie-mtls-missing-cert";
    char ca_file[512] = {0};
    flow_mtls_test_server_t server;
    flowie_mqtt_test_error_state_t state;
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_client_t *client = NULL;
    uint64_t deadline;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.error_count, 0);
    atomic_init(&state.error_status, TURBO_OK);
    check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_equal(flow_mtls_test_server_start(&server, connack, sizeof(connack)), 0);
    config.host = "localhost";
    config.port = server.port;
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.tls.ca_file = ca_file;
    config.on_connect = flowie_mqtt_test_error_connect_completion;
    config.on_error = flowie_mqtt_test_background_error;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.connect_done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      turbo_sleep_ms(1u);
    flow_mtls_test_server_join(&server);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_not_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_OK);
    check_not_equal(server.status, 0);
    check_false(server.peer_verified);
    check_false(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    tls_test_remove_file(ca_file);
  }

  it("MQTT-SEC-001 rejects wrong CA SAN mismatch and untrusted identity before CONNECT") {
    char ca_file[512] = {0};
    char cert_file[512] = {0};
    char key_file[512] = {0};
    char wrong_ca_file[512] = {0};
    char wrong_ca_key_file[512] = {0};
    char untrusted_cert_file[512] = {0};
    char untrusted_key_file[512] = {0};
    check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    check_equal(tls_test_write_self_signed_files(
                     wrong_ca_file, sizeof(wrong_ca_file), wrong_ca_key_file,
                     sizeof(wrong_ca_key_file), "Flowie wrong test CA", 1),
                 0);
    check_equal(tls_test_write_self_signed_files(
                     untrusted_cert_file, sizeof(untrusted_cert_file), untrusted_key_file,
                     sizeof(untrusted_key_file), "Flowie untrusted client", 0),
                 0);
    check_equal(flowie_mqtt_tls_rejection_case("localhost", wrong_ca_file, cert_file, key_file),
                 TURBO_OK);
    check_equal(flowie_mqtt_tls_rejection_case("127.0.0.1", ca_file, cert_file, key_file),
                 TURBO_OK);
    check_equal(flowie_mqtt_tls_rejection_case("localhost", ca_file, untrusted_cert_file,
                                                untrusted_key_file),
                 TURBO_OK);
    tls_test_remove_file(untrusted_key_file);
    tls_test_remove_file(untrusted_cert_file);
    tls_test_remove_file(wrong_ca_key_file);
    tls_test_remove_file(wrong_ca_file);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
    tls_test_remove_file(ca_file);
  }

  it("rejects commands that exceed the configured queue byte budget") {
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_t *client = NULL;
    int completion_count = 0;
    config.host = "127.0.0.1";
    config.command_queue_max_bytes = 1u;
    config.on_ping = flowie_mqtt_test_count_completion;
    config.user_data = &completion_count;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    check_equal(flowie_mqtt_client_ping(client), TURBO_ENOSPC);
    check_equal(completion_count, 0);
    flowie_mqtt_client_destroy(client);
  }

  it("rejects a topic vector atomically when the command queue is too small") {
    static const uint8_t topic_names[2][12] = {"batch/one", "batch/two"};
    static const uint8_t payloads[2][4] = {"one", "two"};
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_publish_topic_t topics[2] = {{0}};
    flowie_mqtt_client_publish_topic_vec_t topic_vec = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    flowie_mqtt_client_t *client = NULL;
    int completion_count = 0;
    config.host = "127.0.0.1";
    config.command_queue_capacity = 1u;
    config.on_publish = flowie_mqtt_test_count_completion;
    config.user_data = &completion_count;
    topics[0].topic = (flowie_mqtt_span_t){topic_names[0], strlen((const char *)topic_names[0])};
    topics[0].payload = (flowie_mqtt_span_t){payloads[0], strlen((const char *)payloads[0])};
    topics[1].topic = (flowie_mqtt_span_t){topic_names[1], strlen((const char *)topic_names[1])};
    topics[1].payload = (flowie_mqtt_span_t){payloads[1], strlen((const char *)payloads[1])};
    topic_vec.version = FLOWIE_MQTT_VERSION_5;
    topic_vec.data = topics;
    topic_vec.count = 2u;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    check_equal(flowie_mqtt_client_publish(client, &topic_vec), TURBO_ENOSPC);
    check_equal(completion_count, 0);
    flowie_mqtt_client_destroy(client);
  }

  it("enforces MQTT 5 broker publish capabilities negotiated in CONNACK") {
    static const uint8_t client_id[] = "flowie-limits-client";
    static const uint8_t topic[] = "limits/a";
    static const uint8_t small_payload[] = "x";
    static const uint8_t large_payload[48] = {0};
    static const uint8_t alias_properties[] = {FLOWIE_MQTT_PROPERTY_TOPIC_ALIAS, 0x00u, 0x02u};
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_client_publish_topic_t topics[4] = {{0}};
    flowie_mqtt_client_publish_topic_vec_t topic_vec = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    flowie_mqtt_limits_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.publish_count, 0);
    atomic_init(&state.disconnect_done, 0);
    state.server_status = TURBO_EBUSY;
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(
        coro_socket_listen_on(server, "127.0.0.1", port, flowie_mqtt_limits_broker_handler, &state),
        TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_limits_connect_completion;
    config.on_publish = flowie_mqtt_limits_publish_completion;
    config.on_disconnect = flowie_mqtt_limits_disconnect_completion;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.connect_done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_OK);

    for (size_t i = 0u; i < 4u; ++i) {
      topics[i].topic = (flowie_mqtt_span_t){topic, sizeof(topic) - 1u};
      topics[i].payload = (flowie_mqtt_span_t){small_payload, sizeof(small_payload) - 1u};
    }
    topics[0].qos = 2u;
    topics[1].retain = 1u;
    topics[2].properties = (flowie_mqtt_span_t){alias_properties, sizeof(alias_properties)};
    topics[3].payload = (flowie_mqtt_span_t){large_payload, sizeof(large_payload)};
    topic_vec.version = FLOWIE_MQTT_VERSION_5;
    topic_vec.data = topics;
    topic_vec.count = 4u;
    check_equal(flowie_mqtt_client_publish(client, &topic_vec), TURBO_OK);
    check_equal(flowie_mqtt_client_disconnect(client, 0u, (flowie_mqtt_span_t){0}), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!state.server_done ||
            atomic_load_explicit(&state.publish_count, memory_order_acquire) != 4 ||
            atomic_load_explicit(&state.disconnect_done, memory_order_acquire) == 0) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_equal(atomic_load_explicit(&state.publish_count, memory_order_acquire), 4);
    check_equal(state.publish_status[0], TURBO_ENOTSUP);
    check_equal(state.publish_status[1], TURBO_ENOTSUP);
    check_equal(state.publish_status[2], TURBO_ENOTSUP);
    check_equal(state.publish_status[3], TURBO_EMSGSIZE);
    check_equal(atomic_load_explicit(&state.disconnect_done, memory_order_acquire), 1);
    check_true(state.server_done);
    check_equal(state.server_status, TURBO_OK);
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("MQTT-SEC-004 completes MQTT 5 enhanced authentication and re-authentication") {
    static const uint8_t client_id[] = "flowie-auth-client";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_auth_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    uint8_t connect_properties[64];
    uint8_t reauth_properties[64];
    size_t connect_properties_size = 0u;
    size_t reauth_properties_size = 0u;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.auth_done, 0);
    atomic_init(&state.auth_status, TURBO_EBUSY);
    atomic_init(&state.disconnect_done, 0);
    atomic_init(&state.challenge_count, 0);
    state.server_status = TURBO_EBUSY;
    check_equal(flowie_mqtt_test_auth_properties_encode(
                     "scram", "client-response", state.initial_response,
                     sizeof(state.initial_response), &state.initial_response_size),
                 TURBO_OK);
    check_equal(flowie_mqtt_test_auth_properties_encode(
                     "scram", "reauth-response", state.reauth_response,
                     sizeof(state.reauth_response), &state.reauth_response_size),
                 TURBO_OK);
    check_equal(flowie_mqtt_test_auth_properties_encode(
                     "scram", "client-first", connect_properties, sizeof(connect_properties),
                     &connect_properties_size),
                 TURBO_OK);
    check_equal(flowie_mqtt_test_auth_properties_encode("scram", "reauth-start", reauth_properties,
                                                         sizeof(reauth_properties),
                                                         &reauth_properties_size),
                 TURBO_OK);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(
        coro_socket_listen_on(server, "127.0.0.1", port, flowie_mqtt_auth_broker_handler, &state),
        TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_auth_connect_completion;
    config.on_auth_challenge = flowie_mqtt_auth_challenge;
    config.on_auth = flowie_mqtt_auth_completion;
    config.on_disconnect = flowie_mqtt_auth_disconnect_completion;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    connect.properties = (flowie_mqtt_span_t){connect_properties, connect_properties_size};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    memset(connect_properties, 0, sizeof(connect_properties));
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.connect_done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_OK);
    check_true(flowie_mqtt_client_is_connected(client));
    check_equal(flowie_mqtt_client_authenticate(
                     client, (flowie_mqtt_span_t){reauth_properties, reauth_properties_size}),
                 TURBO_OK);
    memset(reauth_properties, 0, sizeof(reauth_properties));
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.auth_done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(atomic_load_explicit(&state.auth_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.auth_status, memory_order_relaxed), TURBO_OK);
    check_equal(atomic_load_explicit(&state.challenge_count, memory_order_relaxed), 2);
    check_true(flowie_mqtt_client_is_connected(client));
    check_equal(flowie_mqtt_client_disconnect(client, 0u, (flowie_mqtt_span_t){0}), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!state.server_done ||
            atomic_load_explicit(&state.disconnect_done, memory_order_acquire) == 0) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(state.server_done);
    check_equal(state.server_status, TURBO_OK);
    check_equal(atomic_load_explicit(&state.disconnect_done, memory_order_acquire), 1);
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("MQTT-SEC-004 fails closed when AUTH changes the negotiated method") {
    static const uint8_t client_id[] = "flowie-auth-mismatch";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_auth_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    uint8_t connect_properties[64];
    size_t connect_properties_size = 0u;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.auth_done, 0);
    atomic_init(&state.auth_status, TURBO_EBUSY);
    atomic_init(&state.disconnect_done, 0);
    atomic_init(&state.challenge_count, 0);
    state.server_status = TURBO_EBUSY;
    check_equal(flowie_mqtt_test_auth_properties_encode(
                     "scram", "client-first", connect_properties, sizeof(connect_properties),
                     &connect_properties_size),
                 TURBO_OK);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                       flowie_mqtt_auth_mismatch_broker_handler, &state),
                 TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_auth_connect_completion;
    config.on_auth_challenge = flowie_mqtt_auth_challenge;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    connect.properties = (flowie_mqtt_span_t){connect_properties, connect_properties_size};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (
        (!state.server_done || !atomic_load_explicit(&state.connect_done, memory_order_acquire)) &&
        turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(state.server_done);
    check_equal(state.server_status, TURBO_OK);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_EPROTO);
    check_equal(atomic_load_explicit(&state.challenge_count, memory_order_relaxed), 0);
    check_false(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("MQTT-SEC-004 rejects missing methods and oversized challenge data before callbacks") {
    static const uint8_t client_id[] = "flowie-auth-bounds";
    static const struct {
      void (*handler)(coro_socket_t *, void *);
      size_t max_packet_size;
      int expected_status;
    } cases[] = {{flowie_mqtt_auth_missing_method_broker_handler, 256u, TURBO_EPROTO},
                 {flowie_mqtt_auth_oversized_broker_handler, 64u, TURBO_EMSGSIZE}};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
      flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
      flowie_mqtt_auth_state_t state = {0};
      flowie_mqtt_client_t *client = NULL;
      coro_context_t *server_context = coro_context_create(NULL);
      coro_socket_t *server = NULL;
      uint8_t connect_properties[64];
      size_t connect_properties_size = 0u;
      unsigned short port = flowie_test_port();
      uint64_t deadline;
      check_not_null(server_context);
      check_not_equal(port, 0u);
      atomic_init(&state.connect_done, 0);
      atomic_init(&state.connect_status, TURBO_EBUSY);
      atomic_init(&state.auth_done, 0);
      atomic_init(&state.auth_status, TURBO_EBUSY);
      atomic_init(&state.disconnect_done, 0);
      atomic_init(&state.challenge_count, 0);
      state.server_status = TURBO_EBUSY;
      check_equal(flowie_mqtt_test_auth_properties_encode(
                       "scram", "client-first", connect_properties, sizeof(connect_properties),
                       &connect_properties_size),
                   TURBO_OK);
      server = coro_socket_create_tcpv4(server_context);
      check_not_null(server);
      check_equal(coro_socket_listen_on(server, "127.0.0.1", port, cases[i].handler, &state),
                   TURBO_OK);
      config.host = "127.0.0.1";
      config.port = port;
      config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
      config.max_packet_size = cases[i].max_packet_size;
      config.on_connect = flowie_mqtt_auth_connect_completion;
      config.on_auth_challenge = flowie_mqtt_auth_challenge;
      config.user_data = &state;
      check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
      connect.version = FLOWIE_MQTT_VERSION_5;
      connect.clean_start = 1u;
      connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
      connect.properties = (flowie_mqtt_span_t){connect_properties, connect_properties_size};
      check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
      deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
      while ((!state.server_done ||
              !atomic_load_explicit(&state.connect_done, memory_order_acquire)) &&
             turbo_monotonic_ms() < deadline)
        (void)coro_context_run(server_context, TURBO_RUN_ONCE);
      check_true(state.server_done);
      check_equal(state.server_status, TURBO_OK);
      check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
      check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed),
                   cases[i].expected_status);
      check_equal(atomic_load_explicit(&state.challenge_count, memory_order_relaxed), 0);
      check_false(flowie_mqtt_client_is_connected(client));
      flowie_mqtt_client_destroy(client);
      coro_socket_destroy(server);
      coro_context_destroy(server_context);
    }
  }

  it("MQTT-SEC-004 fails closed when the authentication provider callback fails") {
    static const uint8_t client_id[] = "flowie-auth-provider-failure";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_auth_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    uint8_t connect_properties[64];
    size_t connect_properties_size = 0u;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.auth_done, 0);
    atomic_init(&state.auth_status, TURBO_EBUSY);
    atomic_init(&state.disconnect_done, 0);
    atomic_init(&state.challenge_count, 0);
    state.fail_challenge = 1;
    state.fail_challenge_index = 0;
    state.server_status = TURBO_EBUSY;
    check_equal(flowie_mqtt_test_auth_properties_encode(
                     "scram", "client-first", connect_properties, sizeof(connect_properties),
                     &connect_properties_size),
                 TURBO_OK);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(
        coro_socket_listen_on(server, "127.0.0.1", port, flowie_mqtt_auth_broker_handler, &state),
        TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_auth_connect_completion;
    config.on_auth_challenge = flowie_mqtt_auth_challenge;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    connect.properties = (flowie_mqtt_span_t){connect_properties, connect_properties_size};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.connect_done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_EIO);
    check_equal(atomic_load_explicit(&state.challenge_count, memory_order_relaxed), 1);
    check_false(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("rejects re-authentication on MQTT 3.1.1 without sending AUTH") {
    static const uint8_t client_id[] = "flowie-auth-v3";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_auth_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.auth_done, 0);
    atomic_init(&state.auth_status, TURBO_EBUSY);
    atomic_init(&state.disconnect_done, 0);
    atomic_init(&state.challenge_count, 0);
    state.server_status = TURBO_EBUSY;
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                       flowie_mqtt_auth_v3_broker_handler, &state),
                 TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_auth_plain_connect_completion;
    config.on_auth = flowie_mqtt_auth_completion;
    config.on_disconnect = flowie_mqtt_auth_disconnect_completion;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_3_1_1;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.connect_done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_OK);
    check_equal(flowie_mqtt_client_authenticate(client, (flowie_mqtt_span_t){0}), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.auth_done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(atomic_load_explicit(&state.auth_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.auth_status, memory_order_relaxed), TURBO_ENOTSUP);
    check_true(flowie_mqtt_client_is_connected(client));
    check_equal(flowie_mqtt_client_disconnect(client, 0u, (flowie_mqtt_span_t){0}), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!state.server_done ||
            atomic_load_explicit(&state.disconnect_done, memory_order_acquire) == 0) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(state.server_done);
    check_equal(state.server_status, TURBO_OK);
    check_equal(atomic_load_explicit(&state.disconnect_done, memory_order_acquire), 1);
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("MQTT-NET-005 cancels accepted commands before releasing its worker context") {
    static const uint8_t client_id[] = "flowie-shutdown-client";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_client_t *client = NULL;
    flowie_mqtt_test_shutdown_t shutdown = {0};
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    check_not_null(server_context);
    check_not_equal(port, 0u);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(
        coro_socket_listen_on(server, "127.0.0.1", port, flowie_mqtt_test_broker_handler, NULL),
        TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_test_shutdown_completion;
    config.on_ping = flowie_mqtt_test_shutdown_completion;
    config.user_data = &shutdown;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    check_equal(flowie_mqtt_client_ping(client), TURBO_OK);
    flowie_mqtt_client_destroy(client);
    check_equal(shutdown.count, 2);
    check_equal(shutdown.statuses[0], TURBO_ESHUTDOWN);
    check_equal(shutdown.statuses[1], TURBO_ESHUTDOWN);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("MQTT-NET-005 accepts callback-enqueued work and cancels it once during destroy") {
    static const uint8_t client_id[] = "flowie-reentrant-shutdown";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_reentrant_shutdown_t state;
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    turbo_thread_t server_thread;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    memset(&state, 0, sizeof(state));
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.enqueue_status, TURBO_EBUSY);
    atomic_init(&state.ping_received, 0);
    atomic_init(&state.ping_count, 0);
    atomic_init(&state.ping_status, TURBO_EBUSY);
    check_not_null(server_context);
    check_not_equal(port, 0u);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(
        coro_socket_listen_on(server, "127.0.0.1", port,
                              flowie_mqtt_reentrant_shutdown_broker_handler, &state),
        TURBO_OK);
    coro_context_set_persistent(server_context, 1);
    check_equal(turbo_thread_create(&server_thread, flowie_mqtt_test_context_runner,
                                     server_context),
                 TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_reentrant_connect_completion;
    config.on_ping = flowie_mqtt_reentrant_ping_completion;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!atomic_load_explicit(&state.connect_done, memory_order_acquire) ||
            !atomic_load_explicit(&state.ping_received, memory_order_acquire)) &&
           turbo_monotonic_ms() < deadline)
      turbo_sleep_ms(1u);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_OK);
    check_equal(atomic_load_explicit(&state.enqueue_status, memory_order_relaxed), TURBO_OK);
    check_true(atomic_load_explicit(&state.ping_received, memory_order_acquire));
    flowie_mqtt_client_destroy(client);
    check_equal(atomic_load_explicit(&state.ping_count, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.ping_status, memory_order_relaxed), TURBO_ESHUTDOWN);
    coro_context_set_persistent(server_context, 0);
    coro_context_stop(server_context);
    check_equal(turbo_thread_join(&server_thread), TURBO_OK);
    turbo_thread_destroy(&server_thread);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("keeps a retained idle wake out of an active MQTT 5 QoS 2 exchange") {
    static const uint8_t client_id[] = "flowie-qos2-wake-client";
    static const uint8_t topic[] = "client/qos2/wake";
    static const uint8_t payload[] = "retained-interrupt";
    static const uint8_t inbound_filter[] = "server/qos2/wake";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_topic_handler_t topic_handler = {0};
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_client_publish_topic_t publish_topic = {0};
    flowie_mqtt_client_publish_topic_vec_t publish = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    flowie_mqtt_qos2_wake_state_t state;
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    turbo_thread_t server_thread;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    memset(&state, 0, sizeof(state));
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.publish_count, 0);
    atomic_init(&state.publish_status, TURBO_EBUSY);
    atomic_init(&state.disconnect_done, 0);
    atomic_init(&state.disconnect_status, TURBO_EBUSY);
    atomic_init(&state.inbound_count, 0);
    atomic_init(&state.pulse_count, 0);
    atomic_init(&state.server_status, TURBO_EBUSY);
    atomic_init(&state.server_done, 0);
    check_not_null(server_context);
    check_not_equal(port, 0u);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                       flowie_mqtt_qos2_wake_broker_handler, &state),
                 TURBO_OK);
    coro_context_set_persistent(server_context, 1);
    check_equal(
        turbo_thread_create(&server_thread, flowie_mqtt_test_context_runner, server_context),
        TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_qos2_wake_connect_completion;
    config.on_publish = flowie_mqtt_qos2_wake_publish_completion;
    config.on_disconnect = flowie_mqtt_qos2_wake_disconnect_completion;
    topic_handler.filter =
        (flowie_mqtt_span_t){inbound_filter, sizeof(inbound_filter) - 1u};
    topic_handler.on_message = flowie_mqtt_qos2_wake_on_message;
    config.topic_handlers = (flowie_mqtt_client_topic_handler_map_t){&topic_handler, 1u};
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.connect_done, memory_order_acquire) &&
           turbo_monotonic_ms() < deadline)
      turbo_sleep_ms(1u);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_OK);
    publish_topic.qos = 2u;
    publish_topic.topic = (flowie_mqtt_span_t){topic, sizeof(topic) - 1u};
    publish_topic.payload = (flowie_mqtt_span_t){payload, sizeof(payload) - 1u};
    publish.version = FLOWIE_MQTT_VERSION_5;
    publish.data = &publish_topic;
    publish.count = 1u;
    for (unsigned int i = 0u; i < FLOWIE_MQTT_CLIENT_TEST_QOS2_WAKE_ITERATIONS; ++i) {
      deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
      while (atomic_load_explicit(&state.pulse_count, memory_order_acquire) != (int)i + 1 &&
             turbo_monotonic_ms() < deadline)
        turbo_thread_yield();
      check_equal(atomic_load_explicit(&state.pulse_count, memory_order_acquire), (int)i + 1);
      atomic_store_explicit(&state.publish_status, TURBO_EBUSY, memory_order_relaxed);
      check_equal(flowie_mqtt_client_publish(client, &publish), TURBO_OK);
      deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
      while (atomic_load_explicit(&state.publish_count, memory_order_acquire) != (int)i + 1 &&
             turbo_monotonic_ms() < deadline)
        turbo_thread_yield();
      info("qos2_iteration=%u", i);
      check_equal(atomic_load_explicit(&state.publish_count, memory_order_acquire), (int)i + 1);
      check_equal(atomic_load_explicit(&state.publish_status, memory_order_relaxed), TURBO_OK);
    }
    check_equal(flowie_mqtt_client_disconnect(client, 0u, (flowie_mqtt_span_t){0}), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!atomic_load_explicit(&state.server_done, memory_order_acquire) ||
            !atomic_load_explicit(&state.disconnect_done, memory_order_acquire)) &&
           turbo_monotonic_ms() < deadline)
      turbo_sleep_ms(1u);
    check_true(atomic_load_explicit(&state.server_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.server_status, memory_order_relaxed), TURBO_OK);
    check_true(atomic_load_explicit(&state.disconnect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.disconnect_status, memory_order_relaxed), TURBO_OK);
    check_equal(state.pubrec_sent, FLOWIE_MQTT_CLIENT_TEST_QOS2_WAKE_ITERATIONS);
    check_equal(state.pubrel_received, FLOWIE_MQTT_CLIENT_TEST_QOS2_WAKE_ITERATIONS);
    check_equal(state.pubcomp_sent, FLOWIE_MQTT_CLIENT_TEST_QOS2_WAKE_ITERATIONS);
    check_equal(atomic_load_explicit(&state.inbound_count, memory_order_acquire),
                 (int)FLOWIE_MQTT_CLIENT_TEST_QOS2_WAKE_ITERATIONS * 2);
    flowie_mqtt_client_destroy(client);
    coro_context_set_persistent(server_context, 0);
    coro_context_stop(server_context);
    check_equal(turbo_thread_join(&server_thread), TURBO_OK);
    turbo_thread_destroy(&server_thread);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("reports an unsolicited connection close through the error callback") {
    static const uint8_t client_id[] = "flowie-error-client";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_test_error_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    int error_status;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    atomic_init(&state.connect_done, 0);
    atomic_init(&state.connect_status, TURBO_EBUSY);
    atomic_init(&state.error_count, 0);
    atomic_init(&state.error_status, TURBO_OK);
    state.server_status = TURBO_EBUSY;
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                       flowie_mqtt_test_closing_broker_handler, &state),
                 TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_test_error_connect_completion;
    config.on_error = flowie_mqtt_test_background_error;
    config.user_data = &state;
    check_equal(flowie_mqtt_client_create(&config, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS * 2u;
    while ((!state.server_done ||
            atomic_load_explicit(&state.error_count, memory_order_acquire) == 0) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_true(state.server_done);
    check_equal(state.server_status, TURBO_OK);
    check_true(atomic_load_explicit(&state.connect_done, memory_order_acquire));
    check_equal(atomic_load_explicit(&state.connect_status, memory_order_relaxed), TURBO_OK);
    check_equal(atomic_load_explicit(&state.error_count, memory_order_acquire), 1);
    error_status = atomic_load_explicit(&state.error_status, memory_order_relaxed);
    check_true(error_status == TURBO_EOF || error_status == TURBO_ECONNRESET);
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("reconnects after MQTT 5 server busy without completing the public connect twice") {
    static const uint8_t client_id[] = "flowie-server-busy";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_resilience_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    flowie_mqtt_resilience_state_init(&state);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                     flowie_mqtt_server_busy_then_ready_handler, &state),
                TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_resilience_connect_completion;
    config.user_data = &state;
    resilience.initial_delay_ms = 1u;
    resilience.max_delay_ms = 1u;
    resilience.max_attempts = 2u;
    resilience.on_reconnect = flowie_mqtt_resilience_reconnect_completion;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!flowie_mqtt_client_is_connected(client) ||
            atomic_load_explicit(&state.reconnect_completions, memory_order_acquire) == 0) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    info("broker_connects=%d initial=%d reconnect=%d attempt=%d status=%d reason=%d",
         atomic_load_explicit(&state.broker_connects, memory_order_acquire),
         atomic_load_explicit(&state.initial_completions, memory_order_acquire),
         atomic_load_explicit(&state.reconnect_completions, memory_order_acquire),
         atomic_load_explicit(&state.reconnect_attempt, memory_order_relaxed),
         atomic_load_explicit(&state.reconnect_status, memory_order_relaxed),
         atomic_load_explicit(&state.reconnect_reason, memory_order_relaxed));
    check_equal(atomic_load_explicit(&state.broker_connects, memory_order_acquire), 2);
    check_equal(atomic_load_explicit(&state.initial_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_reason, memory_order_relaxed), 0x89);
    check_equal(atomic_load_explicit(&state.reconnect_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.reconnect_attempt, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&state.reconnect_status, memory_order_relaxed), TURBO_OK);
    check_equal(atomic_load_explicit(&state.reconnect_reason, memory_order_relaxed), 0);
    check_equal(atomic_load_explicit(&state.server_status, memory_order_acquire), TURBO_OK);
    check_true(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("refreshes an expired CONNECT token before reconnecting") {
    static const uint8_t client_id[] = "flowie-expired-token";
    static const uint8_t username[] = "device";
    static const uint8_t initial_token[] = "token-1";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_resilience_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    flowie_mqtt_resilience_state_init(&state);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                     flowie_mqtt_expired_token_then_ready_handler, &state),
                TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_resilience_connect_completion;
    config.user_data = &state;
    resilience.initial_delay_ms = 1u;
    resilience.max_delay_ms = 1u;
    resilience.max_attempts = 2u;
    resilience.refresh_connect = flowie_mqtt_refresh_expired_connect;
    resilience.on_reconnect = flowie_mqtt_resilience_reconnect_completion;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    connect.has_username = 1u;
    connect.has_password = 1u;
    connect.username = (flowie_mqtt_span_t){username, sizeof(username) - 1u};
    connect.password = (flowie_mqtt_span_t){initial_token, sizeof(initial_token) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!flowie_mqtt_client_is_connected(client) ||
            atomic_load_explicit(&state.reconnect_completions, memory_order_acquire) == 0) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_equal(atomic_load_explicit(&state.broker_connects, memory_order_acquire), 2);
    check_equal(atomic_load_explicit(&state.initial_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_reason, memory_order_relaxed), 0x87);
    check_equal(atomic_load_explicit(&state.refresh_calls, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.refreshed_token_seen, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.reconnect_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.reconnect_status, memory_order_relaxed), TURBO_OK);
    check_equal(atomic_load_explicit(&state.reconnect_reason, memory_order_relaxed), 0);
    check_equal(atomic_load_explicit(&state.server_status, memory_order_acquire), TURBO_OK);
    check_true(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("rejects a refreshed CONNECT that changes the selected protocol version") {
    static const uint8_t client_id[] = "flowie-version-conflict";
    static const uint8_t username[] = "device";
    static const uint8_t initial_token[] = "token-1";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_resilience_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    flowie_mqtt_resilience_state_init(&state);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                     flowie_mqtt_expired_token_then_ready_handler, &state),
                TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_resilience_connect_completion;
    config.on_error = flowie_mqtt_resilience_error;
    config.user_data = &state;
    resilience.initial_delay_ms = 1u;
    resilience.max_delay_ms = 1u;
    resilience.max_attempts = 2u;
    resilience.refresh_connect = flowie_mqtt_refresh_with_conflicting_version;
    resilience.on_reconnect = flowie_mqtt_resilience_reconnect_completion;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_OK);
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    connect.has_username = 1u;
    connect.has_password = 1u;
    connect.username = (flowie_mqtt_span_t){username, sizeof(username) - 1u};
    connect.password = (flowie_mqtt_span_t){initial_token, sizeof(initial_token) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(&state.background_errors, memory_order_acquire) == 0 &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    deadline = turbo_monotonic_ms() + 100u;
    while (turbo_monotonic_ms() < deadline) {
      (void)coro_context_run(server_context, TURBO_RUN_NOWAIT);
      turbo_sleep_ms(1u);
    }
    check_equal(atomic_load_explicit(&state.broker_connects, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_reason, memory_order_relaxed), 0x87);
    check_equal(atomic_load_explicit(&state.refresh_calls, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.background_errors, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.background_status, memory_order_relaxed),
                TURBO_EPROTO);
    check_equal(atomic_load_explicit(&state.reconnect_completions, memory_order_acquire), 0);
    check_equal(atomic_load_explicit(&state.server_status, memory_order_acquire), TURBO_OK);
    check_false(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("refreshes credentials after an MQTT 5 not-authorized disconnect") {
    static const uint8_t client_id[] = "flowie-disconnect-refresh";
    static const uint8_t username[] = "device";
    static const uint8_t initial_token[] = "token-1";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_resilience_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    flowie_mqtt_resilience_state_init(&state);
    state.disconnect_reason = 0x87u;
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                     flowie_mqtt_disconnect_then_reconnect_handler, &state),
                TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_resilience_connect_completion;
    config.on_error = flowie_mqtt_resilience_error;
    config.user_data = &state;
    resilience.initial_delay_ms = 1u;
    resilience.max_delay_ms = 1u;
    resilience.max_attempts = 2u;
    resilience.refresh_connect = flowie_mqtt_refresh_expired_connect;
    resilience.on_reconnect = flowie_mqtt_resilience_reconnect_completion;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    connect.has_username = 1u;
    connect.has_password = 1u;
    connect.username = (flowie_mqtt_span_t){username, sizeof(username) - 1u};
    connect.password = (flowie_mqtt_span_t){initial_token, sizeof(initial_token) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!flowie_mqtt_client_is_connected(client) ||
            atomic_load_explicit(&state.reconnect_completions, memory_order_acquire) == 0) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_equal(atomic_load_explicit(&state.broker_connects, memory_order_acquire), 2);
    check_equal(atomic_load_explicit(&state.initial_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_reason, memory_order_relaxed), 0);
    check_equal(atomic_load_explicit(&state.background_errors, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.refresh_calls, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.refreshed_token_seen, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.reconnect_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.reconnect_status, memory_order_relaxed), TURBO_OK);
    check_equal(atomic_load_explicit(&state.reconnect_reason, memory_order_relaxed), 0);
    check_equal(atomic_load_explicit(&state.server_status, memory_order_acquire), TURBO_OK);
    check_true(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("does not reconnect after an MQTT 5 session-taken-over disconnect") {
    static const uint8_t client_id[] = "flowie-session-taken-over";
    static const uint8_t username[] = "device";
    static const uint8_t initial_token[] = "token-1";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_resilience_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    flowie_mqtt_resilience_state_init(&state);
    state.disconnect_reason = 0x8eu;
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                     flowie_mqtt_disconnect_then_reconnect_handler, &state),
                TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_resilience_connect_completion;
    config.on_error = flowie_mqtt_resilience_error;
    config.user_data = &state;
    resilience.initial_delay_ms = 1u;
    resilience.max_delay_ms = 1u;
    resilience.max_attempts = 1u;
    resilience.refresh_connect = flowie_mqtt_refresh_expired_connect;
    resilience.on_reconnect = flowie_mqtt_resilience_reconnect_completion;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    connect.has_username = 1u;
    connect.has_password = 1u;
    connect.username = (flowie_mqtt_span_t){username, sizeof(username) - 1u};
    connect.password = (flowie_mqtt_span_t){initial_token, sizeof(initial_token) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(&state.background_errors, memory_order_acquire) == 0 &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    deadline = turbo_monotonic_ms() + 100u;
    while (turbo_monotonic_ms() < deadline) {
      (void)coro_context_run(server_context, TURBO_RUN_NOWAIT);
      turbo_sleep_ms(1u);
    }
    check_equal(atomic_load_explicit(&state.broker_connects, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_reason, memory_order_relaxed), 0);
    check_equal(atomic_load_explicit(&state.background_errors, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.refresh_calls, memory_order_acquire), 0);
    check_equal(atomic_load_explicit(&state.reconnect_completions, memory_order_acquire), 0);
    check_equal(atomic_load_explicit(&state.server_status, memory_order_acquire), TURBO_OK);
    check_false(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("interrupts an automatic reconnect backoff during destroy") {
    static const uint8_t client_id[] = "flowie-backoff-destroy";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_resilience_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    uint64_t started;
    uint64_t elapsed;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    flowie_mqtt_resilience_state_init(&state);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                     flowie_mqtt_server_busy_then_ready_handler, &state),
                TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_resilience_connect_completion;
    config.user_data = &state;
    resilience.initial_delay_ms = 500u;
    resilience.max_delay_ms = 500u;
    resilience.max_attempts = 2u;
    resilience.on_reconnect = flowie_mqtt_resilience_reconnect_completion;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(&state.initial_completions, memory_order_acquire) == 0 &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_equal(atomic_load_explicit(&state.initial_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_reason, memory_order_relaxed), 0x89);
    turbo_sleep_ms(20u);
    started = turbo_monotonic_ms();
    flowie_mqtt_client_destroy(client);
    elapsed = turbo_monotonic_ms() - started;
    check_true(elapsed < 250u);
    check_equal(atomic_load_explicit(&state.broker_connects, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.reconnect_completions, memory_order_acquire), 0);
    check_equal(atomic_load_explicit(&state.server_status, memory_order_acquire), TURBO_OK);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("reconnects when the network closes during an accepted command") {
    static const uint8_t client_id[] = "flowie-command-close";
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_mqtt_resilience_state_t state = {0};
    flowie_mqtt_client_t *client = NULL;
    coro_context_t *server_context = coro_context_create(NULL);
    coro_socket_t *server = NULL;
    unsigned short port = flowie_test_port();
    uint64_t deadline;
    int ping_status;
    check_not_null(server_context);
    check_not_equal(port, 0u);
    flowie_mqtt_resilience_state_init(&state);
    server = coro_socket_create_tcpv4(server_context);
    check_not_null(server);
    check_equal(coro_socket_listen_on(server, "127.0.0.1", port,
                                     flowie_mqtt_ping_close_then_ready_handler, &state),
                TURBO_OK);
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    config.on_connect = flowie_mqtt_resilience_connect_and_ping;
    config.on_ping = flowie_mqtt_resilience_ping_completion;
    config.user_data = &state;
    resilience.initial_delay_ms = 1u;
    resilience.max_delay_ms = 1u;
    resilience.max_attempts = 2u;
    resilience.on_reconnect = flowie_mqtt_resilience_reconnect_completion;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client), TURBO_OK);
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
    check_equal(flowie_mqtt_client_connect(client, &connect), TURBO_OK);
    deadline = turbo_monotonic_ms() + FLOWIE_MQTT_CLIENT_TEST_TIMEOUT_MS;
    while ((!flowie_mqtt_client_is_connected(client) ||
            atomic_load_explicit(&state.reconnect_completions, memory_order_acquire) == 0) &&
           turbo_monotonic_ms() < deadline)
      (void)coro_context_run(server_context, TURBO_RUN_ONCE);
    check_equal(atomic_load_explicit(&state.broker_connects, memory_order_acquire), 2);
    check_equal(atomic_load_explicit(&state.initial_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.initial_reason, memory_order_relaxed), 0);
    check_equal(atomic_load_explicit(&state.ping_submit_status, memory_order_relaxed), TURBO_OK);
    check_equal(atomic_load_explicit(&state.ping_completions, memory_order_acquire), 1);
    ping_status = atomic_load_explicit(&state.ping_status, memory_order_relaxed);
    check_true(ping_status == TURBO_EOF || ping_status == TURBO_ECONNRESET);
    check_equal(atomic_load_explicit(&state.reconnect_completions, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.reconnect_status, memory_order_relaxed), TURBO_OK);
    check_equal(atomic_load_explicit(&state.reconnect_reason, memory_order_relaxed), 0);
    check_equal(atomic_load_explicit(&state.server_status, memory_order_acquire), TURBO_OK);
    check_true(flowie_mqtt_client_is_connected(client));
    flowie_mqtt_client_destroy(client);
    coro_socket_destroy(server);
    coro_context_destroy(server_context);
  }

  it("uses selected MQTT 3.1 for unspecified packet versions") {
    flowie_mqtt_test_state_t state;
    check_equal(flowie_mqtt_test_run_callbacks(FLOWIE_MQTT_VERSION_3_1, 1,
                                               FLOWIE_MQTT_VERSION_UNSPECIFIED, &state),
                TURBO_OK);
    check_equal(state.publish_count, 2);
    check_equal(state.topics[0], "server/topic/one");
    check_equal(state.payloads[0], "from-broker-one");
    check_equal(state.topics[1], "server/topic/two");
    check_equal(state.payloads[1], "from-broker-two");
    check_equal(state.secondary_match_count, 1);
  }

  it("uses selected MQTT 3.1.1 for unspecified packet versions") {
    flowie_mqtt_test_state_t state;
    check_equal(flowie_mqtt_test_run_callbacks(FLOWIE_MQTT_VERSION_3_1_1, 1,
                                               FLOWIE_MQTT_VERSION_UNSPECIFIED, &state),
                TURBO_OK);
    check_equal(state.publish_count, 2);
    check_equal(state.topics[0], "server/topic/one");
    check_equal(state.payloads[0], "from-broker-one");
    check_equal(state.topics[1], "server/topic/two");
    check_equal(state.payloads[1], "from-broker-two");
    check_equal(state.secondary_match_count, 1);
  }

  it("defaults unspecified packet versions to MQTT 5") {
    flowie_mqtt_test_state_t state;
    check_equal(flowie_mqtt_test_run_callbacks(FLOWIE_MQTT_VERSION_5, 0,
                                               FLOWIE_MQTT_VERSION_UNSPECIFIED, &state),
                TURBO_OK);
    check_equal(state.publish_count, 2);
    check_equal(state.topics[0], "server/topic/one");
    check_equal(state.payloads[0], "from-broker-one");
    check_equal(state.topics[1], "server/topic/two");
    check_equal(state.payloads[1], "from-broker-two");
    check_equal(state.secondary_match_count, 1);
  }
}
