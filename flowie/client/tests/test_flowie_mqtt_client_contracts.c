#include "flowie_mqtt_client.h"
#include "flowie_mqtt_protocol.h"
#include "flowie_connection.h"
#include "mtls_test_server.h"
#include "tls_test_support.h"
#include "tinytest.h"

#include <core/clock.h>
#include <core/error_codes.h>

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

enum {
  FLOWIE_CLIENT_CONTRACT_NEED_MORE = 1,
  FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS = 5000u,
  FLOWIE_CLIENT_CONTRACT_BUFFER_BYTES = 4096u,
  FLOWIE_CLIENT_CONTRACT_QUEUE_BYTES = 4096u,
  FLOWIE_CLIENT_CONTRACT_LARGE_PAYLOAD_BYTES = 3000u
};

static native_io_backend_kind flowie_client_contract_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config flowie_client_contract_network(void) {
  const cnet_client_config config = {
      .backend = flowie_client_contract_backend(),
      .connection_capacity = 4u,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 8192u,
      .receive_buffer_bytes = 8192u,
      .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
      .tls_handshake_timeout_ms = FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS,
      .connect_timeout_ms = FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS,
      .read_timeout_ms = FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS,
      .write_timeout_ms = FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS};
  return config;
}

static int flowie_client_contract_frame_size(const unsigned char *data, size_t size,
                                             size_t *out_size) {
  size_t remaining = 0u;
  size_t multiplier = 1u;
  size_t index;
  if (!data || !out_size) return SALTS_EINVAL;
  *out_size = 0u;
  for (index = 1u; index < size && index <= 4u; ++index) {
    const unsigned char byte = data[index];
    remaining += (size_t)(byte & 0x7fu) * multiplier;
    if ((byte & 0x80u) == 0u) {
      if (remaining > SIZE_MAX - index - 1u) return SALTS_EMSGSIZE;
      *out_size = remaining + index + 1u;
      return *out_size <= size ? SALTS_OK : FLOWIE_CLIENT_CONTRACT_NEED_MORE;
    }
    multiplier *= 128u;
  }
  return size < 5u ? FLOWIE_CLIENT_CONTRACT_NEED_MORE : SALTS_EPROTO;
}

static int flowie_client_contract_wait(atomic_int *value, int expected) {
  const uint64_t deadline = monotonic_ms() + FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) < expected &&
         monotonic_ms() < deadline)
    sleep_ms(1u);
  return atomic_load_explicit(value, memory_order_acquire) >= expected
             ? SALTS_OK
             : SALTS_ETIMEDOUT;
}

typedef struct flowie_client_queue_broker_s {
  flowie_server *server;
  unsigned char input[FLOWIE_CLIENT_CONTRACT_BUFFER_BYTES];
  size_t input_size;
  atomic_int connects;
  atomic_int commands_after_connect;
  atomic_int closes;
  atomic_int error;
} flowie_client_queue_broker;

typedef struct flowie_client_queue_fixture_s {
  flowie_server server;
  flowie_client_queue_broker broker;
  flowie_mqtt_client_t *client;
  int server_initialized;
  int server_started;
  atomic_int connect_completions;
  atomic_int ping_completions;
  atomic_int publish_completions;
  atomic_int connect_status;
  atomic_int ping_status;
  atomic_int publish_status;
  atomic_int errors;
} flowie_client_queue_fixture;

static int flowie_client_queue_open(void *user, flowie_connection connection,
                                    const flowie_peer_info *peer) {
  (void)user;
  (void)connection;
  return peer ? SALTS_OK : SALTS_EINVAL;
}

static int flowie_client_queue_receive(void *user, flowie_connection connection,
                                       const void *data, size_t size) {
  flowie_client_queue_broker *broker = (flowie_client_queue_broker *)user;
  (void)connection;
  if (!broker || !data || size == 0u) return SALTS_EINVAL;
  if (broker->input_size > sizeof(broker->input) ||
      size > sizeof(broker->input) - broker->input_size)
    return SALTS_EMSGSIZE;
  memcpy(broker->input + broker->input_size, data, size);
  broker->input_size += size;
  while (broker->input_size != 0u) {
    size_t frame_size = 0u;
    int rc = flowie_client_contract_frame_size(broker->input, broker->input_size, &frame_size);
    if (rc == FLOWIE_CLIENT_CONTRACT_NEED_MORE) return SALTS_OK;
    if (rc != SALTS_OK) return rc;
    if ((broker->input[0] >> 4u) == FLOWIE_MQTT_PACKET_CONNECT)
      atomic_fetch_add_explicit(&broker->connects, 1, memory_order_release);
    else
      atomic_fetch_add_explicit(&broker->commands_after_connect, 1, memory_order_release);
    memmove(broker->input, broker->input + frame_size, broker->input_size - frame_size);
    broker->input_size -= frame_size;
  }
  return SALTS_OK;
}

static void flowie_client_queue_close(void *user, flowie_connection connection, int status) {
  flowie_client_queue_broker *broker = (flowie_client_queue_broker *)user;
  (void)connection;
  if (!broker) return;
  if (status != SALTS_OK && status != SALTS_EOF && status != SALTS_ECONNRESET &&
      status != SALTS_ESHUTDOWN)
    atomic_store_explicit(&broker->error, status, memory_order_release);
  atomic_fetch_add_explicit(&broker->closes, 1, memory_order_release);
}

static void flowie_client_queue_connect_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_queue_fixture *fixture = (flowie_client_queue_fixture *)user;
  (void)client;
  (void)response;
  atomic_store_explicit(&fixture->connect_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->connect_completions, 1, memory_order_release);
}

static void flowie_client_queue_ping_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_queue_fixture *fixture = (flowie_client_queue_fixture *)user;
  (void)client;
  (void)response;
  atomic_store_explicit(&fixture->ping_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->ping_completions, 1, memory_order_release);
}

static void flowie_client_queue_publish_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_queue_fixture *fixture = (flowie_client_queue_fixture *)user;
  (void)client;
  (void)response;
  atomic_store_explicit(&fixture->publish_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->publish_completions, 1, memory_order_release);
}

static void flowie_client_queue_error(flowie_mqtt_client_t *client, int status, void *user) {
  flowie_client_queue_fixture *fixture = (flowie_client_queue_fixture *)user;
  (void)client;
  (void)status;
  atomic_fetch_add_explicit(&fixture->errors, 1, memory_order_release);
}

static int flowie_client_queue_start(flowie_client_queue_fixture *fixture,
                                     size_t queue_capacity, size_t queue_bytes) {
  static const unsigned char client_id[] = "queue-contract";
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  uint16_t port = 0u;
  int rc;
  if (!fixture) return SALTS_EINVAL;
  atomic_init(&fixture->broker.connects, 0);
  atomic_init(&fixture->broker.commands_after_connect, 0);
  atomic_init(&fixture->broker.closes, 0);
  atomic_init(&fixture->broker.error, SALTS_OK);
  atomic_init(&fixture->connect_completions, 0);
  atomic_init(&fixture->ping_completions, 0);
  atomic_init(&fixture->publish_completions, 0);
  atomic_init(&fixture->connect_status, SALTS_EBUSY);
  atomic_init(&fixture->ping_status, SALTS_EBUSY);
  atomic_init(&fixture->publish_status, SALTS_EBUSY);
  atomic_init(&fixture->errors, 0);
  fixture->broker.server = &fixture->server;
  server_config.transport = TF_NET_TRANSPORT_TCP;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.stream = flowie_client_contract_network();
  server_config.backlog = 2u;
  server_config.command_capacity = 8u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_CONTRACT_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.observer = (flowie_observer){flowie_client_queue_open,
                                             flowie_client_queue_receive,
                                             flowie_client_queue_close, NULL,
                                             &fixture->broker};
  rc = flowie_server_init(&fixture->server, &server_config);
  if (rc != SALTS_OK) return rc;
  fixture->server_initialized = 1;
  rc = flowie_server_start(&fixture->server);
  if (rc != SALTS_OK) return rc;
  fixture->server_started = 1;
  rc = flowie_server_port(&fixture->server, &port);
  if (rc != SALTS_OK) return rc;

  client_config.host = "127.0.0.1";
  client_config.port = port;
  client_config.timeout_ms = FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS;
  client_config.command_queue_capacity = queue_capacity;
  client_config.command_queue_max_bytes = queue_bytes;
  client_config.on_connect = flowie_client_queue_connect_complete;
  client_config.on_ping = flowie_client_queue_ping_complete;
  client_config.on_publish = flowie_client_queue_publish_complete;
  client_config.on_error = flowie_client_queue_error;
  client_config.user_data = fixture;
  rc = flowie_mqtt_client_create(&client_config, &fixture->client);
  if (rc != SALTS_OK) return rc;
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  rc = flowie_mqtt_client_connect(fixture->client, &connect);
  if (rc != SALTS_OK) return rc;
  return flowie_client_contract_wait(&fixture->broker.connects, 1);
}

static int flowie_client_queue_stop(flowie_client_queue_fixture *fixture) {
  int result = SALTS_OK;
  int rc;
  if (!fixture) return SALTS_EINVAL;
  if (fixture->client) {
    flowie_mqtt_client_destroy(fixture->client);
    fixture->client = NULL;
  }
  if (fixture->server_started) {
    rc = flowie_server_stop(&fixture->server, FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS);
    if (result == SALTS_OK) result = rc;
    fixture->server_started = 0;
  }
  if (fixture->server_initialized) {
    rc = flowie_server_destroy(&fixture->server);
    if (result == SALTS_OK) result = rc;
    fixture->server_initialized = 0;
  }
  return result;
}

static const unsigned char flowie_client_auth_start_properties[] = {
    FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD, 0x00u, 0x04u, 't', 'e', 's', 't',
    FLOWIE_MQTT_PROPERTY_AUTHENTICATION_DATA,   0x00u, 0x05u, 's', 't', 'a', 'r', 't'};
static const unsigned char flowie_client_auth_challenge_properties[] = {
    FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD, 0x00u, 0x04u, 't', 'e', 's', 't',
    FLOWIE_MQTT_PROPERTY_AUTHENTICATION_DATA,   0x00u, 0x05u, 'n', 'o', 'n', 'c', 'e'};
static const unsigned char flowie_client_auth_response_properties[] = {
    FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD, 0x00u, 0x04u, 't', 'e', 's', 't',
    FLOWIE_MQTT_PROPERTY_AUTHENTICATION_DATA,   0x00u, 0x05u, 'p', 'r', 'o', 'o', 'f'};
static const unsigned char flowie_client_auth_method_properties[] = {
    FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD, 0x00u, 0x04u, 't', 'e', 's', 't'};
static const unsigned char flowie_client_auth_wrong_method_properties[] = {
    FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD, 0x00u, 0x05u, 'w', 'r', 'o', 'n', 'g'};

typedef enum flowie_client_auth_phase_e {
  FLOWIE_CLIENT_AUTH_EXPECT_CONNECT = 0,
  FLOWIE_CLIENT_AUTH_EXPECT_CONNECT_RESPONSE,
  FLOWIE_CLIENT_AUTH_CONNECTED,
  FLOWIE_CLIENT_AUTH_EXPECT_REAUTH_RESPONSE,
  FLOWIE_CLIENT_AUTH_REAUTHENTICATED
} flowie_client_auth_phase;

typedef struct flowie_client_auth_broker_s {
  flowie_server *server;
  unsigned char input[FLOWIE_CLIENT_CONTRACT_BUFFER_BYTES];
  size_t input_size;
  int wrong_method;
  atomic_int phase;
  atomic_int connects;
  atomic_int connect_responses;
  atomic_int reauth_starts;
  atomic_int reauth_responses;
  atomic_int closes;
  atomic_int error;
} flowie_client_auth_broker;

typedef struct flowie_client_auth_fixture_s {
  flowie_server server;
  flowie_client_auth_broker broker;
  flowie_mqtt_client_t *client;
  int server_initialized;
  int server_started;
  int fail_challenge_index;
  atomic_int challenges;
  atomic_int connect_completions;
  atomic_int auth_completions;
  atomic_int connect_status;
  atomic_int auth_status;
  atomic_int connect_response_type;
  atomic_int auth_response_type;
  atomic_int auth_response_reason;
  atomic_int errors;
} flowie_client_auth_fixture;

static int flowie_client_contract_span_equal(flowie_mqtt_span_t actual,
                                             const unsigned char *expected,
                                             size_t expected_size) {
  return actual.size == expected_size &&
         (expected_size == 0u ||
          (actual.data && memcmp(actual.data, expected, expected_size) == 0));
}

static int flowie_client_auth_send(flowie_client_auth_broker *broker,
                                   flowie_connection connection,
                                   flowie_mqtt_packet_type_t type, uint8_t reason,
                                   const unsigned char *properties, size_t property_size) {
  flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  unsigned char output[128];
  size_t written = 0u;
  int rc;
  packet.version = FLOWIE_MQTT_VERSION_5;
  packet.type = type;
  packet.reason_code = reason;
  packet.properties = (flowie_mqtt_span_t){properties, property_size};
  rc = flowie_mqtt_control_packet_encode(&packet, output, sizeof(output), &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  return flowie_server_send(broker->server, connection, output, written);
}

static int flowie_client_auth_open(void *user, flowie_connection connection,
                                   const flowie_peer_info *peer) {
  (void)user;
  (void)connection;
  return peer ? SALTS_OK : SALTS_EINVAL;
}

static int flowie_client_auth_receive(void *user, flowie_connection connection,
                                      const void *data, size_t size) {
  flowie_client_auth_broker *broker = (flowie_client_auth_broker *)user;
  flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  int rc = SALTS_OK;
  if (!broker || !data || size == 0u) return SALTS_EINVAL;
  if (broker->input_size > sizeof(broker->input) ||
      size > sizeof(broker->input) - broker->input_size)
    return SALTS_EMSGSIZE;
  memcpy(broker->input + broker->input_size, data, size);
  broker->input_size += size;
  options.version = FLOWIE_MQTT_VERSION_5;
  while (broker->input_size != 0u) {
    flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
    size_t frame_size = 0u;
    size_t consumed = 0u;
    rc = flowie_client_contract_frame_size(broker->input, broker->input_size, &frame_size);
    if (rc == FLOWIE_CLIENT_CONTRACT_NEED_MORE) return SALTS_OK;
    if (rc != SALTS_OK) goto fail;
    rc = flowie_mqtt_packet_parse(broker->input, frame_size, &options, &packet,
                                  &consumed, NULL);
    if (rc != FLOWIE_MQTT_PARSE_OK || consumed != frame_size) {
      rc = SALTS_EPROTO;
      goto fail;
    }
    if (packet.type == FLOWIE_MQTT_PACKET_CONNECT) {
      flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
      if (atomic_load_explicit(&broker->phase, memory_order_relaxed) !=
              FLOWIE_CLIENT_AUTH_EXPECT_CONNECT ||
          flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK ||
          !flowie_client_contract_span_equal(connect.properties.values,
                                             flowie_client_auth_start_properties,
                                             sizeof(flowie_client_auth_start_properties))) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      atomic_fetch_add_explicit(&broker->connects, 1, memory_order_release);
      atomic_store_explicit(&broker->phase,
                            FLOWIE_CLIENT_AUTH_EXPECT_CONNECT_RESPONSE,
                            memory_order_relaxed);
      rc = flowie_client_auth_send(
          broker, connection, FLOWIE_MQTT_PACKET_AUTH, 0x18u,
          broker->wrong_method ? flowie_client_auth_wrong_method_properties
                               : flowie_client_auth_challenge_properties,
          broker->wrong_method ? sizeof(flowie_client_auth_wrong_method_properties)
                               : sizeof(flowie_client_auth_challenge_properties));
      if (rc != SALTS_OK) goto fail;
    } else if (packet.type == FLOWIE_MQTT_PACKET_AUTH) {
      flowie_mqtt_control_packet_view_t auth = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
      const int phase = atomic_load_explicit(&broker->phase, memory_order_relaxed);
      if (flowie_mqtt_control_packet_parse(&packet, &auth) != FLOWIE_MQTT_PARSE_OK) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      if (phase == FLOWIE_CLIENT_AUTH_EXPECT_CONNECT_RESPONSE &&
          auth.reason_code == 0x18u &&
          flowie_client_contract_span_equal(auth.properties.values,
                                            flowie_client_auth_response_properties,
                                            sizeof(flowie_client_auth_response_properties))) {
        atomic_fetch_add_explicit(&broker->connect_responses, 1, memory_order_release);
        atomic_store_explicit(&broker->phase, FLOWIE_CLIENT_AUTH_CONNECTED,
                              memory_order_relaxed);
        rc = flowie_client_auth_send(broker, connection, FLOWIE_MQTT_PACKET_CONNACK,
                                     0u, flowie_client_auth_method_properties,
                                     sizeof(flowie_client_auth_method_properties));
      } else if (phase == FLOWIE_CLIENT_AUTH_CONNECTED && auth.reason_code == 0x19u &&
                 flowie_client_contract_span_equal(
                     auth.properties.values, flowie_client_auth_start_properties,
                     sizeof(flowie_client_auth_start_properties))) {
        atomic_fetch_add_explicit(&broker->reauth_starts, 1, memory_order_release);
        atomic_store_explicit(&broker->phase,
                              FLOWIE_CLIENT_AUTH_EXPECT_REAUTH_RESPONSE,
                              memory_order_relaxed);
        rc = flowie_client_auth_send(broker, connection, FLOWIE_MQTT_PACKET_AUTH,
                                     0x18u, flowie_client_auth_challenge_properties,
                                     sizeof(flowie_client_auth_challenge_properties));
      } else if (phase == FLOWIE_CLIENT_AUTH_EXPECT_REAUTH_RESPONSE &&
                 auth.reason_code == 0x18u &&
                 flowie_client_contract_span_equal(
                     auth.properties.values, flowie_client_auth_response_properties,
                     sizeof(flowie_client_auth_response_properties))) {
        atomic_fetch_add_explicit(&broker->reauth_responses, 1, memory_order_release);
        atomic_store_explicit(&broker->phase,
                              FLOWIE_CLIENT_AUTH_REAUTHENTICATED,
                              memory_order_relaxed);
        rc = flowie_client_auth_send(broker, connection, FLOWIE_MQTT_PACKET_AUTH,
                                     0u, flowie_client_auth_method_properties,
                                     sizeof(flowie_client_auth_method_properties));
      } else {
        rc = SALTS_EPROTO;
      }
      if (rc != SALTS_OK) goto fail;
    } else {
      rc = SALTS_EPROTO;
      goto fail;
    }
    memmove(broker->input, broker->input + frame_size, broker->input_size - frame_size);
    broker->input_size -= frame_size;
  }
  return SALTS_OK;

fail:
  atomic_store_explicit(&broker->error, rc, memory_order_release);
  return rc;
}

static void flowie_client_auth_close(void *user, flowie_connection connection, int status) {
  flowie_client_auth_broker *broker = (flowie_client_auth_broker *)user;
  (void)connection;
  (void)status;
  if (broker)
    atomic_fetch_add_explicit(&broker->closes, 1, memory_order_release);
}

static int flowie_client_auth_challenge(
    flowie_mqtt_client_t *client,
    const flowie_mqtt_control_packet_view_t *challenge,
    flowie_mqtt_client_auth_response_t *response, void *user) {
  flowie_client_auth_fixture *fixture = (flowie_client_auth_fixture *)user;
  const int index = atomic_fetch_add_explicit(&fixture->challenges, 1,
                                               memory_order_acq_rel) + 1;
  (void)client;
  if (!challenge || !response || challenge->type != FLOWIE_MQTT_PACKET_AUTH ||
      challenge->reason_code != 0x18u ||
      !flowie_client_contract_span_equal(
          challenge->properties.values, flowie_client_auth_challenge_properties,
          sizeof(flowie_client_auth_challenge_properties)))
    return SALTS_EPROTO;
  if (fixture->fail_challenge_index == index) return SALTS_EPERM;
  response->size = sizeof(*response);
  response->reason_code = 0x18u;
  response->properties = (flowie_mqtt_span_t){
      flowie_client_auth_response_properties,
      sizeof(flowie_client_auth_response_properties)};
  return SALTS_OK;
}

static void flowie_client_auth_connect_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_auth_fixture *fixture = (flowie_client_auth_fixture *)user;
  (void)client;
  atomic_store_explicit(&fixture->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&fixture->connect_response_type,
                        response ? (int)response->type : 0, memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->connect_completions, 1, memory_order_release);
}

static void flowie_client_auth_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_auth_fixture *fixture = (flowie_client_auth_fixture *)user;
  (void)client;
  atomic_store_explicit(&fixture->auth_status, status, memory_order_relaxed);
  atomic_store_explicit(&fixture->auth_response_type,
                        response ? (int)response->type : 0, memory_order_relaxed);
  atomic_store_explicit(&fixture->auth_response_reason,
                        response ? (int)response->reason_code : -1,
                        memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->auth_completions, 1, memory_order_release);
}

static void flowie_client_auth_error(flowie_mqtt_client_t *client, int status, void *user) {
  flowie_client_auth_fixture *fixture = (flowie_client_auth_fixture *)user;
  (void)client;
  (void)status;
  atomic_fetch_add_explicit(&fixture->errors, 1, memory_order_release);
}

static int flowie_client_auth_start(flowie_client_auth_fixture *fixture,
                                    int wrong_method, int install_callback,
                                    int fail_challenge_index) {
  static const unsigned char client_id[] = "auth-contract";
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  uint16_t port = 0u;
  int rc;
  if (!fixture) return SALTS_EINVAL;
  fixture->broker.server = &fixture->server;
  fixture->broker.wrong_method = wrong_method;
  fixture->fail_challenge_index = fail_challenge_index;
  atomic_init(&fixture->broker.phase, FLOWIE_CLIENT_AUTH_EXPECT_CONNECT);
  atomic_init(&fixture->broker.connects, 0);
  atomic_init(&fixture->broker.connect_responses, 0);
  atomic_init(&fixture->broker.reauth_starts, 0);
  atomic_init(&fixture->broker.reauth_responses, 0);
  atomic_init(&fixture->broker.closes, 0);
  atomic_init(&fixture->broker.error, SALTS_OK);
  atomic_init(&fixture->challenges, 0);
  atomic_init(&fixture->connect_completions, 0);
  atomic_init(&fixture->auth_completions, 0);
  atomic_init(&fixture->connect_status, SALTS_EBUSY);
  atomic_init(&fixture->auth_status, SALTS_EBUSY);
  atomic_init(&fixture->connect_response_type, 0);
  atomic_init(&fixture->auth_response_type, 0);
  atomic_init(&fixture->auth_response_reason, -1);
  atomic_init(&fixture->errors, 0);

  server_config.transport = TF_NET_TRANSPORT_TCP;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.stream = flowie_client_contract_network();
  server_config.backlog = 2u;
  server_config.command_capacity = 16u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_CONTRACT_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.observer = (flowie_observer){flowie_client_auth_open,
                                             flowie_client_auth_receive,
                                             flowie_client_auth_close, NULL,
                                             &fixture->broker};
  rc = flowie_server_init(&fixture->server, &server_config);
  if (rc != SALTS_OK) return rc;
  fixture->server_initialized = 1;
  rc = flowie_server_start(&fixture->server);
  if (rc != SALTS_OK) return rc;
  fixture->server_started = 1;
  rc = flowie_server_port(&fixture->server, &port);
  if (rc != SALTS_OK) return rc;

  client_config.host = "127.0.0.1";
  client_config.port = port;
  client_config.timeout_ms = FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS;
  client_config.on_connect = flowie_client_auth_connect_complete;
  client_config.on_auth = flowie_client_auth_complete;
  client_config.on_auth_challenge =
      install_callback ? flowie_client_auth_challenge : NULL;
  client_config.on_error = flowie_client_auth_error;
  client_config.user_data = fixture;
  rc = flowie_mqtt_client_create(&client_config, &fixture->client);
  if (rc != SALTS_OK) return rc;
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  connect.properties = (flowie_mqtt_span_t){flowie_client_auth_start_properties,
                                            sizeof(flowie_client_auth_start_properties)};
  return flowie_mqtt_client_connect(fixture->client, &connect);
}

static int flowie_client_auth_stop(flowie_client_auth_fixture *fixture) {
  int result = SALTS_OK;
  int rc;
  if (!fixture) return SALTS_EINVAL;
  if (fixture->client) {
    flowie_mqtt_client_destroy(fixture->client);
    fixture->client = NULL;
  }
  if (fixture->server_started) {
    rc = flowie_server_stop(&fixture->server, FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS);
    if (result == SALTS_OK) result = rc;
    fixture->server_started = 0;
  }
  if (fixture->server_initialized) {
    rc = flowie_server_destroy(&fixture->server);
    if (result == SALTS_OK) result = rc;
    fixture->server_initialized = 0;
  }
  return result;
}

typedef struct flowie_client_mtls_fixture_s {
  flow_mtls_test_server_t server;
  flowie_mqtt_client_t *client;
  char ca_file[512];
  char cert_file[512];
  char key_file[512];
  atomic_int completions;
  atomic_int status;
  atomic_int response_type;
  atomic_int errors;
} flowie_client_mtls_fixture;

static void flowie_client_mtls_connect_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_mtls_fixture *fixture = (flowie_client_mtls_fixture *)user;
  (void)client;
  atomic_store_explicit(&fixture->status, status, memory_order_relaxed);
  atomic_store_explicit(&fixture->response_type,
                        response ? (int)response->type : 0, memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->completions, 1, memory_order_release);
}

static void flowie_client_mtls_error(flowie_mqtt_client_t *client, int status, void *user) {
  flowie_client_mtls_fixture *fixture = (flowie_client_mtls_fixture *)user;
  (void)client;
  (void)status;
  atomic_fetch_add_explicit(&fixture->errors, 1, memory_order_release);
}

static int flowie_client_mtls_start(flowie_client_mtls_fixture *fixture,
                                    int with_client_identity) {
  static const unsigned char connack[] = {0x20u, 0x03u, 0x00u, 0x00u, 0x00u};
  static const unsigned char client_id[] = "mtls-contract";
  flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  int rc;
  if (!fixture) return SALTS_EINVAL;
  atomic_init(&fixture->completions, 0);
  atomic_init(&fixture->status, SALTS_EBUSY);
  atomic_init(&fixture->response_type, 0);
  atomic_init(&fixture->errors, 0);
  if (tls_test_write_ca_file(fixture->ca_file, sizeof(fixture->ca_file)) != 0)
    return SALTS_EIO;
  if (with_client_identity &&
      tls_test_write_server_files(fixture->cert_file, sizeof(fixture->cert_file),
                                  fixture->key_file, sizeof(fixture->key_file)) != 0)
    return SALTS_EIO;
  if (flow_mtls_test_server_start(&fixture->server, connack, sizeof(connack)) != 0)
    return SALTS_EIO;
  config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
  config.host = "localhost";
  config.port = fixture->server.port;
  config.timeout_ms = FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS;
  config.tls.ca_file = fixture->ca_file;
  config.tls.cert_file = with_client_identity ? fixture->cert_file : NULL;
  config.tls.key_file = with_client_identity ? fixture->key_file : NULL;
  config.on_connect = flowie_client_mtls_connect_complete;
  config.on_error = flowie_client_mtls_error;
  config.user_data = fixture;
  rc = flowie_mqtt_client_create(&config, &fixture->client);
  if (rc != SALTS_OK) return rc;
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  return flowie_mqtt_client_connect(fixture->client, &connect);
}

static int flowie_client_mtls_finish(flowie_client_mtls_fixture *fixture) {
  int rc = SALTS_OK;
  flow_mtls_test_server_release(&fixture->server);
  if (fixture->client) {
    rc = flowie_mqtt_client_try_destroy(fixture->client, FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS);
    if (rc == SALTS_OK) fixture->client = NULL;
  }
  flow_mtls_test_server_join(&fixture->server);
  return rc;
}

static int flowie_client_mtls_cleanup(flowie_client_mtls_fixture *fixture) {
  const int rc = flowie_client_mtls_finish(fixture);
  tls_test_remove_file(fixture->key_file);
  tls_test_remove_file(fixture->cert_file);
  tls_test_remove_file(fixture->ca_file);
  fixture->key_file[0] = '\0';
  fixture->cert_file[0] = '\0';
  fixture->ca_file[0] = '\0';
  return rc;
}

typedef enum flowie_client_command_broker_mode_e {
  FLOWIE_CLIENT_COMMAND_BROKER_ROUND_TRIP = 0,
  FLOWIE_CLIENT_COMMAND_BROKER_QOS2_LIMIT
} flowie_client_command_broker_mode;

typedef struct flowie_client_command_broker_s {
  flowie_server *server;
  unsigned char input[FLOWIE_CLIENT_CONTRACT_BUFFER_BYTES];
  size_t input_size;
  flowie_client_command_broker_mode mode;
  atomic_int connects;
  atomic_int subscribes;
  atomic_int unsubscribes;
  atomic_int pubrecs;
  atomic_int closes;
  atomic_int error;
} flowie_client_command_broker;

typedef struct flowie_client_command_fixture_s {
  flowie_server server;
  flowie_client_command_broker broker;
  flowie_mqtt_client_t *client;
  int server_initialized;
  int server_started;
  unsigned char exact_filter[32];
  unsigned char wildcard_filter[32];
  flowie_mqtt_client_topic_handler_t handlers[2];
  int handler_order[2];
  atomic_int connect_completions;
  atomic_int subscribe_completions;
  atomic_int unsubscribe_completions;
  atomic_int message_completions;
  atomic_int errors;
  atomic_int connect_status;
  atomic_int subscribe_status;
  atomic_int unsubscribe_status;
  atomic_int error_status;
  atomic_int subscribe_response_type;
  atomic_int unsubscribe_response_type;
  atomic_int subscribe_reason_count;
  atomic_int unsubscribe_reason_count;
} flowie_client_command_fixture;

static const unsigned char flowie_client_command_subscription_filter[] = "sensor/#";
static const unsigned char flowie_client_command_topic[] = "sensor/room/temp";
static const unsigned char flowie_client_command_payload[] = "21.5";

static int flowie_client_command_send_control(
    flowie_client_command_broker *broker, flowie_connection connection,
    flowie_mqtt_packet_type_t type, uint16_t packet_id,
    const unsigned char *reasons, size_t reason_count) {
  flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  unsigned char output[128];
  size_t written = 0u;
  int rc;
  packet.version = FLOWIE_MQTT_VERSION_5;
  packet.type = type;
  packet.packet_id = packet_id;
  packet.reason_codes = (flowie_mqtt_span_t){reasons, reason_count};
  rc = flowie_mqtt_control_packet_encode(&packet, output, sizeof(output), &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  return flowie_server_send(broker->server, connection, output, written);
}

static int flowie_client_command_send_publish(
    flowie_client_command_broker *broker, flowie_connection connection,
    uint8_t qos, uint16_t packet_id) {
  flowie_mqtt_publish_packet_t publish = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
  unsigned char output[256];
  size_t written = 0u;
  int rc;
  publish.version = FLOWIE_MQTT_VERSION_5;
  publish.qos = qos;
  publish.packet_id = packet_id;
  publish.topic = (flowie_mqtt_span_t){flowie_client_command_topic,
                                       sizeof(flowie_client_command_topic) - 1u};
  publish.payload = (flowie_mqtt_span_t){flowie_client_command_payload,
                                         sizeof(flowie_client_command_payload) - 1u};
  rc = flowie_mqtt_publish_packet_encode(&publish, output, sizeof(output), &written);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  return flowie_server_send(broker->server, connection, output, written);
}

static int flowie_client_command_open(void *user, flowie_connection connection,
                                      const flowie_peer_info *peer) {
  (void)user;
  (void)connection;
  return peer ? SALTS_OK : SALTS_EINVAL;
}

static int flowie_client_command_receive(void *user, flowie_connection connection,
                                         const void *data, size_t size) {
  static const unsigned char subscribe_reason[] = {0x01u};
  static const unsigned char unsubscribe_reason[] = {0x00u};
  flowie_client_command_broker *broker = (flowie_client_command_broker *)user;
  flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  int rc = SALTS_OK;
  if (!broker || !data || size == 0u) return SALTS_EINVAL;
  if (broker->input_size > sizeof(broker->input) ||
      size > sizeof(broker->input) - broker->input_size)
    return SALTS_EMSGSIZE;
  memcpy(broker->input + broker->input_size, data, size);
  broker->input_size += size;
  options.version = FLOWIE_MQTT_VERSION_5;
  while (broker->input_size != 0u) {
    flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
    size_t frame_size = 0u;
    size_t consumed = 0u;
    rc = flowie_client_contract_frame_size(broker->input, broker->input_size, &frame_size);
    if (rc == FLOWIE_CLIENT_CONTRACT_NEED_MORE) return SALTS_OK;
    if (rc != SALTS_OK) goto fail;
    rc = flowie_mqtt_packet_parse(broker->input, frame_size, &options, &packet,
                                  &consumed, NULL);
    if (rc != FLOWIE_MQTT_PARSE_OK || consumed != frame_size) {
      rc = SALTS_EPROTO;
      goto fail;
    }
    if (packet.type == FLOWIE_MQTT_PACKET_CONNECT) {
      atomic_fetch_add_explicit(&broker->connects, 1, memory_order_release);
      rc = flowie_client_command_send_control(
          broker, connection, FLOWIE_MQTT_PACKET_CONNACK, 0u, NULL, 0u);
    } else if (packet.type == FLOWIE_MQTT_PACKET_SUBSCRIBE) {
      flowie_mqtt_subscribe_view_t subscribe = FLOWIE_MQTT_SUBSCRIBE_VIEW_INIT;
      flowie_mqtt_subscription_iterator_t iterator =
          FLOWIE_MQTT_SUBSCRIPTION_ITERATOR_INIT;
      flowie_mqtt_subscription_view_t entry = {0};
      if (flowie_mqtt_subscribe_parse(&packet, &subscribe) != FLOWIE_MQTT_PARSE_OK ||
          subscribe.entry_count != 1u ||
          flowie_mqtt_subscription_iterator_init(&packet, &subscribe, &iterator) !=
              FLOWIE_MQTT_PARSE_OK ||
          flowie_mqtt_subscription_iterator_next(&iterator, &entry) !=
              FLOWIE_MQTT_PARSE_OK ||
          !flowie_client_contract_span_equal(
              entry.filter, flowie_client_command_subscription_filter,
              sizeof(flowie_client_command_subscription_filter) - 1u)) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      atomic_fetch_add_explicit(&broker->subscribes, 1, memory_order_release);
      rc = flowie_client_command_send_control(
          broker, connection, FLOWIE_MQTT_PACKET_SUBACK, subscribe.packet_id,
          subscribe_reason, sizeof(subscribe_reason));
      if (rc == SALTS_OK && broker->mode == FLOWIE_CLIENT_COMMAND_BROKER_ROUND_TRIP)
        rc = flowie_client_command_send_publish(broker, connection, 0u, 0u);
      if (rc == SALTS_OK && broker->mode == FLOWIE_CLIENT_COMMAND_BROKER_QOS2_LIMIT)
        rc = flowie_client_command_send_publish(broker, connection, 2u, 1u);
      if (rc == SALTS_OK && broker->mode == FLOWIE_CLIENT_COMMAND_BROKER_QOS2_LIMIT)
        rc = flowie_client_command_send_publish(broker, connection, 2u, 2u);
    } else if (packet.type == FLOWIE_MQTT_PACKET_UNSUBSCRIBE) {
      flowie_mqtt_unsubscribe_view_t unsubscribe = FLOWIE_MQTT_UNSUBSCRIBE_VIEW_INIT;
      flowie_mqtt_topic_filter_iterator_t iterator =
          FLOWIE_MQTT_TOPIC_FILTER_ITERATOR_INIT;
      flowie_mqtt_span_t filter = {0};
      if (flowie_mqtt_unsubscribe_parse(&packet, &unsubscribe) !=
              FLOWIE_MQTT_PARSE_OK ||
          unsubscribe.filter_count != 1u ||
          flowie_mqtt_topic_filter_iterator_init(&unsubscribe, &iterator) !=
              FLOWIE_MQTT_PARSE_OK ||
          flowie_mqtt_topic_filter_iterator_next(&iterator, &filter) !=
              FLOWIE_MQTT_PARSE_OK ||
          !flowie_client_contract_span_equal(
              filter, flowie_client_command_subscription_filter,
              sizeof(flowie_client_command_subscription_filter) - 1u)) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      atomic_fetch_add_explicit(&broker->unsubscribes, 1, memory_order_release);
      rc = flowie_client_command_send_control(
          broker, connection, FLOWIE_MQTT_PACKET_UNSUBACK, unsubscribe.packet_id,
          unsubscribe_reason, sizeof(unsubscribe_reason));
    } else if (packet.type == FLOWIE_MQTT_PACKET_PUBREC) {
      flowie_mqtt_control_packet_view_t pubrec = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
      if (flowie_mqtt_control_packet_parse(&packet, &pubrec) != FLOWIE_MQTT_PARSE_OK ||
          pubrec.packet_id != 1u) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      atomic_fetch_add_explicit(&broker->pubrecs, 1, memory_order_release);
      rc = SALTS_OK;
    } else if (packet.type == FLOWIE_MQTT_PACKET_DISCONNECT) {
      rc = SALTS_OK;
    } else {
      rc = SALTS_EPROTO;
    }
    if (rc != SALTS_OK) goto fail;
    memmove(broker->input, broker->input + frame_size, broker->input_size - frame_size);
    broker->input_size -= frame_size;
  }
  return SALTS_OK;

fail:
  atomic_store_explicit(&broker->error, rc, memory_order_release);
  return rc;
}

static void flowie_client_command_close(void *user, flowie_connection connection,
                                        int status) {
  flowie_client_command_broker *broker = (flowie_client_command_broker *)user;
  (void)connection;
  (void)status;
  if (broker)
    atomic_fetch_add_explicit(&broker->closes, 1, memory_order_release);
}

static int flowie_client_command_record_message(
    flowie_client_command_fixture *fixture,
    const flowie_mqtt_publish_view_t *message, int handler) {
  int position;
  if (!fixture || !message ||
      !flowie_client_contract_span_equal(
          message->topic, flowie_client_command_topic,
          sizeof(flowie_client_command_topic) - 1u) ||
      !flowie_client_contract_span_equal(
          message->payload, flowie_client_command_payload,
          sizeof(flowie_client_command_payload) - 1u))
    return SALTS_EPROTO;
  position = atomic_load_explicit(&fixture->message_completions, memory_order_relaxed);
  if (position < 0 || position >= 2) return SALTS_ENOSPC;
  fixture->handler_order[position] = handler;
  atomic_fetch_add_explicit(&fixture->message_completions, 1, memory_order_release);
  return SALTS_OK;
}

static int flowie_client_command_first_handler(
    flowie_mqtt_client_t *client, const flowie_mqtt_publish_view_t *message,
    void *user) {
  (void)client;
  return flowie_client_command_record_message(
      (flowie_client_command_fixture *)user, message, 1);
}

static int flowie_client_command_second_handler(
    flowie_mqtt_client_t *client, const flowie_mqtt_publish_view_t *message,
    void *user) {
  (void)client;
  return flowie_client_command_record_message(
      (flowie_client_command_fixture *)user, message, 2);
}

static void flowie_client_command_connect_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_command_fixture *fixture = (flowie_client_command_fixture *)user;
  (void)client;
  (void)response;
  atomic_store_explicit(&fixture->connect_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->connect_completions, 1, memory_order_release);
}

static void flowie_client_command_subscribe_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_command_fixture *fixture = (flowie_client_command_fixture *)user;
  (void)client;
  atomic_store_explicit(&fixture->subscribe_status, status, memory_order_relaxed);
  atomic_store_explicit(&fixture->subscribe_response_type,
                        response ? (int)response->type : 0, memory_order_relaxed);
  atomic_store_explicit(&fixture->subscribe_reason_count,
                        response ? (int)response->reason_codes.size : 0,
                        memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->subscribe_completions, 1, memory_order_release);
}

static void flowie_client_command_unsubscribe_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_command_fixture *fixture = (flowie_client_command_fixture *)user;
  (void)client;
  atomic_store_explicit(&fixture->unsubscribe_status, status, memory_order_relaxed);
  atomic_store_explicit(&fixture->unsubscribe_response_type,
                        response ? (int)response->type : 0, memory_order_relaxed);
  atomic_store_explicit(&fixture->unsubscribe_reason_count,
                        response ? (int)response->reason_codes.size : 0,
                        memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->unsubscribe_completions, 1,
                            memory_order_release);
}

static void flowie_client_command_error(flowie_mqtt_client_t *client, int status,
                                        void *user) {
  flowie_client_command_fixture *fixture = (flowie_client_command_fixture *)user;
  (void)client;
  atomic_store_explicit(&fixture->error_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&fixture->errors, 1, memory_order_release);
}

static int flowie_client_command_start(
    flowie_client_command_fixture *fixture,
    flowie_client_command_broker_mode mode) {
  static const unsigned char client_id[] = "command-contract";
  static const unsigned char exact_filter[] = "sensor/room/temp";
  static const unsigned char wildcard_filter[] = "sensor/+/temp";
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  uint16_t port = 0u;
  int rc;
  if (!fixture) return SALTS_EINVAL;
  fixture->broker.server = &fixture->server;
  fixture->broker.mode = mode;
  atomic_init(&fixture->broker.connects, 0);
  atomic_init(&fixture->broker.subscribes, 0);
  atomic_init(&fixture->broker.unsubscribes, 0);
  atomic_init(&fixture->broker.pubrecs, 0);
  atomic_init(&fixture->broker.closes, 0);
  atomic_init(&fixture->broker.error, SALTS_OK);
  atomic_init(&fixture->connect_completions, 0);
  atomic_init(&fixture->subscribe_completions, 0);
  atomic_init(&fixture->unsubscribe_completions, 0);
  atomic_init(&fixture->message_completions, 0);
  atomic_init(&fixture->errors, 0);
  atomic_init(&fixture->connect_status, SALTS_EBUSY);
  atomic_init(&fixture->subscribe_status, SALTS_EBUSY);
  atomic_init(&fixture->unsubscribe_status, SALTS_EBUSY);
  atomic_init(&fixture->error_status, SALTS_EBUSY);
  atomic_init(&fixture->subscribe_response_type, 0);
  atomic_init(&fixture->unsubscribe_response_type, 0);
  atomic_init(&fixture->subscribe_reason_count, 0);
  atomic_init(&fixture->unsubscribe_reason_count, 0);
  memcpy(fixture->exact_filter, exact_filter, sizeof(exact_filter));
  memcpy(fixture->wildcard_filter, wildcard_filter, sizeof(wildcard_filter));
  fixture->handlers[0].filter = (flowie_mqtt_span_t){fixture->exact_filter,
                                                     sizeof(exact_filter) - 1u};
  fixture->handlers[0].on_message = flowie_client_command_first_handler;
  fixture->handlers[1].filter = (flowie_mqtt_span_t){fixture->wildcard_filter,
                                                     sizeof(wildcard_filter) - 1u};
  fixture->handlers[1].on_message = flowie_client_command_second_handler;

  server_config.transport = TF_NET_TRANSPORT_TCP;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.stream = flowie_client_contract_network();
  server_config.backlog = 2u;
  server_config.command_capacity = 16u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_CONTRACT_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.observer = (flowie_observer){flowie_client_command_open,
                                             flowie_client_command_receive,
                                             flowie_client_command_close, NULL,
                                             &fixture->broker};
  rc = flowie_server_init(&fixture->server, &server_config);
  if (rc != SALTS_OK) return rc;
  fixture->server_initialized = 1;
  rc = flowie_server_start(&fixture->server);
  if (rc != SALTS_OK) return rc;
  fixture->server_started = 1;
  rc = flowie_server_port(&fixture->server, &port);
  if (rc != SALTS_OK) return rc;

  client_config.host = "127.0.0.1";
  client_config.port = port;
  client_config.timeout_ms = FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS;
  client_config.max_inbound_qos2 =
      mode == FLOWIE_CLIENT_COMMAND_BROKER_QOS2_LIMIT
          ? 1u
          : FLOWIE_MQTT_CLIENT_DEFAULT_MAX_INBOUND_QOS2;
  client_config.topic_handlers = (flowie_mqtt_client_topic_handler_map_t){
      fixture->handlers,
      mode == FLOWIE_CLIENT_COMMAND_BROKER_QOS2_LIMIT ? 1u : 2u};
  client_config.on_connect = flowie_client_command_connect_complete;
  client_config.on_subscribe = flowie_client_command_subscribe_complete;
  client_config.on_unsubscribe = flowie_client_command_unsubscribe_complete;
  client_config.on_error = flowie_client_command_error;
  client_config.user_data = fixture;
  rc = flowie_mqtt_client_create(&client_config, &fixture->client);
  if (rc != SALTS_OK) return rc;
  memset(fixture->exact_filter, 'x', sizeof(exact_filter) - 1u);
  memset(fixture->wildcard_filter, 'x', sizeof(wildcard_filter) - 1u);
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  rc = flowie_mqtt_client_connect(fixture->client, &connect);
  if (rc != SALTS_OK) return rc;
  rc = flowie_client_contract_wait(&fixture->connect_completions, 1);
  if (rc != SALTS_OK) return rc;
  return atomic_load_explicit(&fixture->connect_status, memory_order_acquire);
}

static int flowie_client_command_stop(flowie_client_command_fixture *fixture) {
  int result = SALTS_OK;
  int rc;
  if (!fixture) return SALTS_EINVAL;
  if (fixture->client) {
    flowie_mqtt_client_destroy(fixture->client);
    fixture->client = NULL;
  }
  if (fixture->server_started) {
    rc = flowie_server_stop(&fixture->server, FLOWIE_CLIENT_CONTRACT_TIMEOUT_MS);
    if (result == SALTS_OK) result = rc;
    fixture->server_started = 0;
  }
  if (fixture->server_initialized) {
    rc = flowie_server_destroy(&fixture->server);
    if (result == SALTS_OK) result = rc;
    fixture->server_initialized = 0;
  }
  return result;
}

static flowie_mqtt_client_config_t flowie_client_contract_config(void) {
  flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  config.host = "127.0.0.1";
  config.port = 1;
  return config;
}

static int flowie_client_contract_message(flowie_mqtt_client_t *client,
                                          const flowie_mqtt_publish_view_t *message,
                                          void *user) {
  (void)client;
  (void)message;
  (void)user;
  return SALTS_OK;
}

typedef struct flowie_client_version_probe_s {
  atomic_int completions;
} flowie_client_version_probe;

static void flowie_client_version_publish_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_version_probe *probe = (flowie_client_version_probe *)user;
  (void)client;
  (void)status;
  (void)response;
  atomic_fetch_add_explicit(&probe->completions, 1, memory_order_release);
}

spec("Flowie MQTT client bounded command admission") {
  static flowie_client_queue_fixture fixture;

  before_each() { memset(&fixture, 0, sizeof(fixture)); }

  after_each() { check_equal(flowie_client_queue_stop(&fixture), SALTS_OK); }

  it("rejects a multi-topic PUBLISH atomically when command slots are full") {
    static const unsigned char topic_a[] = "queue/a";
    static const unsigned char topic_b[] = "queue/b";
    flowie_mqtt_client_publish_topic_t topics[2] = {{0}};
    flowie_mqtt_client_publish_topic_vec_t batch = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    flowie_mqtt_client_publish_topic_vec_t single = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    topics[0].topic = (flowie_mqtt_span_t){topic_a, sizeof(topic_a) - 1u};
    topics[1].topic = (flowie_mqtt_span_t){topic_b, sizeof(topic_b) - 1u};
    batch.version = FLOWIE_MQTT_VERSION_5;
    batch.data = topics;
    batch.count = 2u;
    single.version = FLOWIE_MQTT_VERSION_5;
    single.data = topics;
    single.count = 1u;
    check_equal(flowie_client_queue_start(&fixture, 2u, 16384u), SALTS_OK);
    check_equal(flowie_mqtt_client_ping(fixture.client), SALTS_OK);
    check_equal(flowie_mqtt_client_publish(fixture.client, &batch), SALTS_ENOSPC);
    check_equal(flowie_mqtt_client_publish(fixture.client, &single), SALTS_OK);
    check_equal(flowie_mqtt_client_ping(fixture.client), SALTS_ENOSPC);
    check_equal(flowie_client_queue_stop(&fixture), SALTS_OK);
    check_equal(atomic_load(&fixture.connect_completions), 1);
    check_equal(atomic_load(&fixture.ping_completions), 1);
    check_equal(atomic_load(&fixture.publish_completions), 1);
    check_equal(atomic_load(&fixture.connect_status), SALTS_ESHUTDOWN);
    check_equal(atomic_load(&fixture.ping_status), SALTS_ESHUTDOWN);
    check_equal(atomic_load(&fixture.publish_status), SALTS_ESHUTDOWN);
    check_equal(atomic_load(&fixture.broker.commands_after_connect), 0);
    check_equal(atomic_load(&fixture.broker.error), SALTS_OK);
    check_equal(atomic_load(&fixture.errors), 0);
  }

  it("rejects a multi-topic PUBLISH atomically when owned bytes are full") {
    static const unsigned char topic_a[] = "bytes/a";
    static const unsigned char topic_b[] = "bytes/b";
    static unsigned char payload[FLOWIE_CLIENT_CONTRACT_LARGE_PAYLOAD_BYTES];
    flowie_mqtt_client_publish_topic_t topics[2] = {{0}};
    flowie_mqtt_client_publish_topic_vec_t batch = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    flowie_mqtt_client_publish_topic_vec_t single = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    memset(payload, 0x5au, sizeof(payload));
    topics[0].topic = (flowie_mqtt_span_t){topic_a, sizeof(topic_a) - 1u};
    topics[0].payload = (flowie_mqtt_span_t){payload, sizeof(payload)};
    topics[1].topic = (flowie_mqtt_span_t){topic_b, sizeof(topic_b) - 1u};
    topics[1].payload = (flowie_mqtt_span_t){payload, sizeof(payload)};
    batch.version = FLOWIE_MQTT_VERSION_5;
    batch.data = topics;
    batch.count = 2u;
    single.version = FLOWIE_MQTT_VERSION_5;
    single.data = topics;
    single.count = 1u;
    check_equal(flowie_client_queue_start(
                    &fixture, 4u, FLOWIE_CLIENT_CONTRACT_QUEUE_BYTES),
                SALTS_OK);
    check_equal(flowie_mqtt_client_publish(fixture.client, &batch), SALTS_ENOSPC);
    check_equal(flowie_mqtt_client_publish(fixture.client, &single), SALTS_OK);
    check_equal(flowie_client_queue_stop(&fixture), SALTS_OK);
    check_equal(atomic_load(&fixture.connect_completions), 1);
    check_equal(atomic_load(&fixture.publish_completions), 1);
    check_equal(atomic_load(&fixture.publish_status), SALTS_ESHUTDOWN);
    check_equal(atomic_load(&fixture.broker.commands_after_connect), 0);
    check_equal(atomic_load(&fixture.broker.error), SALTS_OK);
    check_equal(atomic_load(&fixture.errors), 0);
  }
}

spec("Flowie MQTT client enhanced authentication") {
  static flowie_client_auth_fixture fixture;

  before_each() { memset(&fixture, 0, sizeof(fixture)); }

  after_each() { check_equal(flowie_client_auth_stop(&fixture), SALTS_OK); }

  it("completes CONNECT challenge and online re-authentication") {
    const flowie_mqtt_span_t properties = {
        flowie_client_auth_start_properties,
        sizeof(flowie_client_auth_start_properties)};
    check_equal(flowie_client_auth_start(&fixture, 0, 1, 0), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.connect_completions, 1), SALTS_OK);
    check_equal(atomic_load(&fixture.connect_status), SALTS_OK);
    check_equal(atomic_load(&fixture.connect_response_type), FLOWIE_MQTT_PACKET_CONNACK);
    check_equal(atomic_load(&fixture.challenges), 1);
    check_equal(atomic_load(&fixture.broker.connect_responses), 1);
    check_equal(flowie_mqtt_client_is_connected(fixture.client), 1);
    check_equal(flowie_mqtt_client_authenticate(fixture.client, properties), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.auth_completions, 1), SALTS_OK);
    check_equal(atomic_load(&fixture.auth_status), SALTS_OK);
    check_equal(atomic_load(&fixture.auth_response_type), FLOWIE_MQTT_PACKET_AUTH);
    check_equal(atomic_load(&fixture.auth_response_reason), 0);
    check_equal(atomic_load(&fixture.challenges), 2);
    check_equal(atomic_load(&fixture.broker.reauth_starts), 1);
    check_equal(atomic_load(&fixture.broker.reauth_responses), 1);
    check_equal(atomic_load(&fixture.broker.error), SALTS_OK);
    check_equal(flowie_mqtt_client_is_connected(fixture.client), 1);
  }

  it("rejects a mismatched AUTH method before invoking the challenge callback") {
    check_equal(flowie_client_auth_start(&fixture, 1, 1, 0), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.connect_completions, 1), SALTS_OK);
    check_equal(atomic_load(&fixture.connect_status), SALTS_EPROTO);
    check_equal(atomic_load(&fixture.challenges), 0);
    check_equal(atomic_load(&fixture.broker.connect_responses), 0);
    check_equal(flowie_mqtt_client_is_connected(fixture.client), 0);
  }

  it("fails CONNECT closed when no challenge callback is configured") {
    check_equal(flowie_client_auth_start(&fixture, 0, 0, 0), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.connect_completions, 1), SALTS_OK);
    check_equal(atomic_load(&fixture.connect_status), SALTS_ENOTSUP);
    check_equal(atomic_load(&fixture.challenges), 0);
    check_equal(atomic_load(&fixture.broker.connect_responses), 0);
    check_equal(flowie_mqtt_client_is_connected(fixture.client), 0);
  }

  it("fails re-authentication closed when the second challenge is rejected") {
    const flowie_mqtt_span_t properties = {
        flowie_client_auth_start_properties,
        sizeof(flowie_client_auth_start_properties)};
    check_equal(flowie_client_auth_start(&fixture, 0, 1, 2), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.connect_completions, 1), SALTS_OK);
    check_equal(atomic_load(&fixture.connect_status), SALTS_OK);
    check_equal(flowie_mqtt_client_authenticate(fixture.client, properties), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.auth_completions, 1), SALTS_OK);
    check_equal(atomic_load(&fixture.auth_status), SALTS_EPERM);
    check_equal(atomic_load(&fixture.challenges), 2);
    check_equal(atomic_load(&fixture.broker.reauth_starts), 1);
    check_equal(atomic_load(&fixture.broker.reauth_responses), 0);
    check_equal(flowie_mqtt_client_is_connected(fixture.client), 0);
  }
}

spec("Flowie MQTT client TLS identity") {
  static flowie_client_mtls_fixture fixture;

  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    fixture.server.listener = FLOW_MTLS_TEST_INVALID_SOCKET;
  }

  after_each() { check_equal(flowie_client_mtls_cleanup(&fixture), SALTS_OK); }

  it("presents a configured client certificate to an mTLS broker") {
    check_equal(flowie_client_mtls_start(&fixture, 1), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.completions, 1), SALTS_OK);
    check_equal(flowie_client_mtls_finish(&fixture), SALTS_OK);
    check_equal(atomic_load(&fixture.status), SALTS_OK);
    check_equal(atomic_load(&fixture.response_type), FLOWIE_MQTT_PACKET_CONNACK);
    check_equal(fixture.server.status, 0);
    check_equal(fixture.server.peer_verified, 1);
    check_greater(fixture.server.request_size, (size_t)0u);
    check_equal(fixture.server.request[0] >> 4u, FLOWIE_MQTT_PACKET_CONNECT);
  }

  it("fails closed when an mTLS broker requires an absent client certificate") {
    check_equal(flowie_client_mtls_start(&fixture, 0), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.completions, 1), SALTS_OK);
    check_equal(flowie_client_mtls_finish(&fixture), SALTS_OK);
    check_not_equal(atomic_load(&fixture.status), SALTS_OK);
    check_equal(fixture.server.handshake_rejected, 1);
    check_equal(fixture.server.peer_verified, 0);
    check_equal(fixture.server.request_size, (size_t)0u);
  }
}

spec("Flowie MQTT client subscribe and inbound dispatch") {
  static flowie_client_command_fixture fixture;

  before_each() { memset(&fixture, 0, sizeof(fixture)); }

  after_each() { check_equal(flowie_client_command_stop(&fixture), SALTS_OK); }

  it("owns subscribe inputs and dispatches overlapping handlers in map order") {
    unsigned char subscribe_filter[] = "sensor/#";
    unsigned char unsubscribe_filter[] = "sensor/#";
    flowie_mqtt_subscription_t subscription = {0};
    flowie_mqtt_subscribe_packet_t subscribe = FLOWIE_MQTT_SUBSCRIBE_PACKET_INIT;
    flowie_mqtt_span_t filter = {unsubscribe_filter,
                                 sizeof(unsubscribe_filter) - 1u};
    flowie_mqtt_unsubscribe_packet_t unsubscribe =
        FLOWIE_MQTT_UNSUBSCRIBE_PACKET_INIT;
    check_equal(flowie_client_command_start(
                    &fixture, FLOWIE_CLIENT_COMMAND_BROKER_ROUND_TRIP),
                SALTS_OK);
    subscription.filter = (flowie_mqtt_span_t){subscribe_filter,
                                                sizeof(subscribe_filter) - 1u};
    subscription.qos = 1u;
    subscribe.version = FLOWIE_MQTT_VERSION_5;
    subscribe.subscriptions = &subscription;
    subscribe.subscription_count = 1u;
    check_equal(flowie_mqtt_client_subscribe(fixture.client, &subscribe), SALTS_OK);
    memset(subscribe_filter, 'x', sizeof(subscribe_filter) - 1u);
    check_equal(flowie_client_contract_wait(&fixture.subscribe_completions, 1), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.message_completions, 2), SALTS_OK);
    check_equal(atomic_load(&fixture.subscribe_status), SALTS_OK);
    check_equal(atomic_load(&fixture.subscribe_response_type), FLOWIE_MQTT_PACKET_SUBACK);
    check_equal(atomic_load(&fixture.subscribe_reason_count), 1);
    check_equal(fixture.handler_order[0], 1);
    check_equal(fixture.handler_order[1], 2);
    check_equal(atomic_load(&fixture.broker.subscribes), 1);

    unsubscribe.version = FLOWIE_MQTT_VERSION_5;
    unsubscribe.filters = &filter;
    unsubscribe.filter_count = 1u;
    check_equal(flowie_mqtt_client_unsubscribe(fixture.client, &unsubscribe), SALTS_OK);
    memset(unsubscribe_filter, 'x', sizeof(unsubscribe_filter) - 1u);
    check_equal(flowie_client_contract_wait(&fixture.unsubscribe_completions, 1), SALTS_OK);
    check_equal(atomic_load(&fixture.unsubscribe_status), SALTS_OK);
    check_equal(atomic_load(&fixture.unsubscribe_response_type), FLOWIE_MQTT_PACKET_UNSUBACK);
    check_equal(atomic_load(&fixture.unsubscribe_reason_count), 1);
    check_equal(atomic_load(&fixture.broker.unsubscribes), 1);
    check_equal(atomic_load(&fixture.broker.error), SALTS_OK);
    check_equal(atomic_load(&fixture.errors), 0);
  }

  it("closes with ENOSPC when the inbound QoS2 ownership limit is reached") {
    unsigned char subscribe_filter[] = "sensor/#";
    flowie_mqtt_subscription_t subscription = {0};
    flowie_mqtt_subscribe_packet_t subscribe = FLOWIE_MQTT_SUBSCRIBE_PACKET_INIT;
    check_equal(flowie_client_command_start(
                    &fixture, FLOWIE_CLIENT_COMMAND_BROKER_QOS2_LIMIT),
                SALTS_OK);
    subscription.filter = (flowie_mqtt_span_t){subscribe_filter,
                                                sizeof(subscribe_filter) - 1u};
    subscription.qos = 2u;
    subscribe.version = FLOWIE_MQTT_VERSION_5;
    subscribe.subscriptions = &subscription;
    subscribe.subscription_count = 1u;
    check_equal(flowie_mqtt_client_subscribe(fixture.client, &subscribe), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.subscribe_completions, 1), SALTS_OK);
    check_equal(flowie_client_contract_wait(&fixture.errors, 1), SALTS_OK);
    check_equal(atomic_load(&fixture.subscribe_status), SALTS_OK);
    check_equal(atomic_load(&fixture.message_completions), 1);
    check_equal(atomic_load(&fixture.error_status), SALTS_ENOSPC);
    check_equal(atomic_load(&fixture.broker.pubrecs), 1);
    check_equal(flowie_mqtt_client_is_connected(fixture.client), 0);
  }
}

spec("Flowie MQTT client configuration and version admission") {
  it("rejects invalid bounded-resource and TLS configurations before allocation") {
    flowie_mqtt_client_config_t config;
    flowie_mqtt_client_t *client;

    config = flowie_client_contract_config();
    config.max_packet_size = 1u;
    client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_EINVAL);
    check_null(client);

    config = flowie_client_contract_config();
    config.max_inbound_qos2 = 0u;
    client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_EINVAL);
    check_null(client);

    config = flowie_client_contract_config();
    config.command_queue_capacity = SIZE_MAX;
    client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_EINVAL);
    check_null(client);

    config = flowie_client_contract_config();
    config.command_queue_max_bytes = SIZE_MAX;
    client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_EINVAL);
    check_null(client);

    config = flowie_client_contract_config();
    config.stream_recv_buffer_bytes = 1u;
    client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_ERANGE);
    check_null(client);

    config = flowie_client_contract_config();
    config.tls.ca_file = "ca.pem";
    client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_EINVAL);
    check_null(client);

    config = flowie_client_contract_config();
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
    config.tls.cert_file = "client.pem";
    client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_EINVAL);
    check_null(client);
  }

  it("rejects duplicate topic handlers and invalid reconnect delay ordering") {
    static const unsigned char filter[] = "sensor/+";
    flowie_mqtt_client_topic_handler_t handlers[2] = {{0}};
    flowie_mqtt_client_resilience_config_t resilience =
        FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
    flowie_mqtt_client_config_t config = flowie_client_contract_config();
    flowie_mqtt_client_t *client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    handlers[0].filter = (flowie_mqtt_span_t){filter, sizeof(filter) - 1u};
    handlers[0].on_message = flowie_client_contract_message;
    handlers[1] = handlers[0];
    config.topic_handlers =
        (flowie_mqtt_client_topic_handler_map_t){handlers, 2u};
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_EINVAL);
    check_null(client);

    config = flowie_client_contract_config();
    resilience.initial_delay_ms = 20u;
    resilience.max_delay_ms = 10u;
    client = (flowie_mqtt_client_t *)(uintptr_t)1u;
    check_equal(flowie_mqtt_client_create_ex(&config, &resilience, &client),
                SALTS_EINVAL);
    check_null(client);
  }

  it("locks an explicitly selected version when a versioned command is admitted") {
    static const unsigned char topic_name[] = "version/topic";
    flowie_client_version_probe probe;
    flowie_mqtt_client_config_t config = flowie_client_contract_config();
    flowie_mqtt_client_publish_topic_t topic = {0};
    flowie_mqtt_client_publish_topic_vec_t topics =
        FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    flowie_mqtt_client_t *client = NULL;
    atomic_init(&probe.completions, 0);
    config.on_publish = flowie_client_version_publish_complete;
    config.user_data = &probe;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
    check_equal(flowie_mqtt_client_set_version(NULL, FLOWIE_MQTT_VERSION_5),
                SALTS_EINVAL);
    check_equal(flowie_mqtt_client_set_version(
                    client, FLOWIE_MQTT_VERSION_UNSPECIFIED),
                SALTS_EINVAL);
    check_equal(flowie_mqtt_client_set_version(
                    client, (flowie_mqtt_version_t)0x7f),
                SALTS_EINVAL);
    check_equal(flowie_mqtt_client_set_version(
                    client, FLOWIE_MQTT_VERSION_3_1_1),
                SALTS_OK);
    topic.topic = (flowie_mqtt_span_t){topic_name, sizeof(topic_name) - 1u};
    topics.version = FLOWIE_MQTT_VERSION_UNSPECIFIED;
    topics.data = &topic;
    topics.count = 1u;
    check_equal(flowie_mqtt_client_publish(client, &topics), SALTS_OK);
    check_equal(flowie_mqtt_client_set_version(client, FLOWIE_MQTT_VERSION_5),
                SALTS_EALREADY);
    flowie_mqtt_client_destroy(client);
    client = NULL;
    check_equal(atomic_load(&probe.completions), 1);
    check_equal(flowie_mqtt_client_is_connected(NULL), 0);
  }
}
