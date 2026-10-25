#include "flowie_mqtt_client.h"
#include "flowie_mqtt_protocol.h"
#if defined(FLOWIE_CLIENT_FAULT_TEST)
#include "flowie_mqtt_client_fault_test.h"
#endif
#include "flowie_connection.h"
#include "tls_test_support.h"
#include "mtls_test_server.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <string.h>

enum {
  FLOWIE_CLIENT_TRANSPORT_NEED_MORE = 1,
  FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS = 5000u,
  FLOWIE_CLIENT_TRANSPORT_SHORT_TIMEOUT_MS = 100u,
  FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES = 4096u
};

typedef enum flowie_client_transport_connack_mode {
  FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID = 0,
  FLOWIE_CLIENT_TRANSPORT_CONNACK_SILENT,
  FLOWIE_CLIENT_TRANSPORT_CONNACK_UNEXPECTED_PACKET,
  FLOWIE_CLIENT_TRANSPORT_CONNACK_ABRUPT_CLOSE
} flowie_client_transport_connack_mode;

typedef struct flowie_client_transport_broker {
  flowie_server *server;
  unsigned char input[FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES];
  size_t input_size;
  atomic_int opens;
  atomic_int connects;
  atomic_int publishes;
  atomic_int last_publish_qos;
  atomic_int pings;
  atomic_int disconnects;
  atomic_int closes;
  atomic_int error;
  flowie_client_transport_connack_mode connack_mode;
  int expected_close_status;
} flowie_client_transport_broker;

typedef struct flowie_client_transport_probe {
  atomic_int done;
  atomic_int connect_status;
  atomic_int ping_status;
  atomic_int disconnect_status;
  atomic_int submit_status;
  atomic_int errors;
  atomic_int reconnect_attempts;
  atomic_int callback_destroy_status;
} flowie_client_transport_probe;

static native_io_backend_kind flowie_client_transport_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int flowie_client_transport_frame_size(const unsigned char *data, size_t size,
                                              size_t *out_size) {
  size_t remaining = 0u;
  size_t multiplier = 1u;
  size_t index;
  if (data == NULL || out_size == NULL) return SALTS_EINVAL;
  *out_size = 0u;
  for (index = 1u; index < size && index <= 4u; ++index) {
    const unsigned char byte = data[index];
    remaining += (size_t)(byte & 0x7fu) * multiplier;
    if ((byte & 0x80u) == 0u) {
      if (remaining > SIZE_MAX - index - 1u) return SALTS_EMSGSIZE;
      *out_size = remaining + index + 1u;
      return *out_size <= size ? SALTS_OK : FLOWIE_CLIENT_TRANSPORT_NEED_MORE;
    }
    multiplier *= 128u;
  }
  return size < 5u ? FLOWIE_CLIENT_TRANSPORT_NEED_MORE : SALTS_EPROTO;
}

static int flowie_client_transport_open(void *user, flowie_connection connection,
                                        const flowie_peer_info *peer) {
  flowie_client_transport_broker *broker = (flowie_client_transport_broker *)user;
  (void)connection;
  if (broker == NULL || peer == NULL) return SALTS_EINVAL;
  atomic_fetch_add_explicit(&broker->opens, 1, memory_order_release);
  return SALTS_OK;
}

static int flowie_client_transport_receive(void *user, flowie_connection connection,
                                           const void *data, size_t size) {
  static const unsigned char connack[] = {0x20u, 0x03u, 0x00u, 0x00u, 0x00u};
  static const unsigned char pingresp[] = {0xd0u, 0x00u};
  flowie_client_transport_broker *broker = (flowie_client_transport_broker *)user;
  if (broker == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  if (broker->input_size > sizeof(broker->input) ||
      size > sizeof(broker->input) - broker->input_size)
    return SALTS_EMSGSIZE;
  memcpy(broker->input + broker->input_size, data, size);
  broker->input_size += size;
  while (broker->input_size != 0u) {
    size_t frame_size = 0u;
    int status =
        flowie_client_transport_frame_size(broker->input, broker->input_size, &frame_size);
    if (status == FLOWIE_CLIENT_TRANSPORT_NEED_MORE) return SALTS_OK;
    if (status != SALTS_OK) return status;
    switch (broker->input[0] >> 4u) {
    case FLOWIE_MQTT_PACKET_CONNECT:
      atomic_fetch_add_explicit(&broker->connects, 1, memory_order_release);
      if (broker->connack_mode == FLOWIE_CLIENT_TRANSPORT_CONNACK_SILENT)
        status = SALTS_OK;
      else if (broker->connack_mode == FLOWIE_CLIENT_TRANSPORT_CONNACK_UNEXPECTED_PACKET)
        status = flowie_server_send(broker->server, connection, pingresp, sizeof(pingresp));
      else if (broker->connack_mode == FLOWIE_CLIENT_TRANSPORT_CONNACK_ABRUPT_CLOSE)
        status = SALTS_ECONNABORTED;
      else
        status = flowie_server_send(broker->server, connection, connack, sizeof(connack));
      break;
    case FLOWIE_MQTT_PACKET_PINGREQ:
      atomic_fetch_add_explicit(&broker->pings, 1, memory_order_release);
      status = flowie_server_send(broker->server, connection, pingresp, sizeof(pingresp));
      break;
    case FLOWIE_MQTT_PACKET_PUBLISH:
      /* Deliberately withhold PUBACK/PUBREC: the sender must still own the
       * accepted QoS1/2 command when shutdown interrupts the exchange. */
      atomic_store_explicit(&broker->last_publish_qos,
                            (broker->input[0] >> 1u) & 3u, memory_order_release);
      atomic_fetch_add_explicit(&broker->publishes, 1, memory_order_relaxed);
      status = SALTS_OK;
      break;
    case FLOWIE_MQTT_PACKET_DISCONNECT:
      atomic_fetch_add_explicit(&broker->disconnects, 1, memory_order_release);
      status = SALTS_OK;
      break;
    default: status = SALTS_EPROTO; break;
    }
    if (status != SALTS_OK) return status;
    memmove(broker->input, broker->input + frame_size, broker->input_size - frame_size);
    broker->input_size -= frame_size;
  }
  return SALTS_OK;
}

static void flowie_client_transport_close(void *user, flowie_connection connection, int status) {
  flowie_client_transport_broker *broker = (flowie_client_transport_broker *)user;
  (void)connection;
  if (broker == NULL) return;
  if (status != broker->expected_close_status &&
      status != SALTS_EOF && status != SALTS_ECONNRESET)
    atomic_store_explicit(&broker->error, status, memory_order_release);
  atomic_fetch_add_explicit(&broker->closes, 1, memory_order_release);
}

static void flowie_client_transport_connect_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_transport_probe *probe = (flowie_client_transport_probe *)user;
  int submit_status = SALTS_EPROTO;
  if (status == SALTS_OK && response != NULL && response->type == FLOWIE_MQTT_PACKET_CONNACK &&
      response->reason_code == 0u)
    submit_status = flowie_mqtt_client_ping(client);
  atomic_store_explicit(&probe->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&probe->submit_status, submit_status, memory_order_relaxed);
  if (status != SALTS_OK || submit_status != SALTS_OK)
    atomic_store_explicit(&probe->done, 1, memory_order_release);
}

/* A Client callback belongs to its Worker and must never join itself.
 * An external owner must perform the actual bounded native/CHttp drain. */
static void flowie_client_transport_connect_self_stop(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_transport_probe *probe = (flowie_client_transport_probe *)user;
  int stop_status;
  int ping_status;
  if (status != SALTS_OK || response == NULL ||
      response->type != FLOWIE_MQTT_PACKET_CONNACK || response->reason_code != 0u) {
    atomic_store_explicit(&probe->connect_status,
                          status == SALTS_OK ? SALTS_EPROTO : status, memory_order_relaxed);
    atomic_store_explicit(&probe->done, 1, memory_order_release);
    return;
  }
  stop_status = flowie_mqtt_client_try_destroy(client, 0u);
  ping_status = flowie_mqtt_client_ping(client);
  atomic_store_explicit(&probe->callback_destroy_status, stop_status, memory_order_relaxed);
  atomic_store_explicit(&probe->submit_status, ping_status, memory_order_relaxed);
  atomic_store_explicit(&probe->connect_status, status, memory_order_relaxed);
  atomic_store_explicit(&probe->done, 1, memory_order_release);
}

static void flowie_client_transport_ping_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_transport_probe *probe = (flowie_client_transport_probe *)user;
  const flowie_mqtt_span_t empty = {NULL, 0u};
  const int submit_status =
      status == SALTS_OK ? flowie_mqtt_client_disconnect(client, 0u, empty) : status;
  (void)response;
  atomic_store_explicit(&probe->ping_status, status, memory_order_relaxed);
  atomic_store_explicit(&probe->submit_status, submit_status, memory_order_relaxed);
  if (status != SALTS_OK || submit_status != SALTS_OK)
    atomic_store_explicit(&probe->done, 1, memory_order_release);
}

static void flowie_client_transport_disconnect_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_transport_probe *probe = (flowie_client_transport_probe *)user;
  (void)client;
  (void)response;
  atomic_store_explicit(&probe->disconnect_status, status, memory_order_relaxed);
  atomic_store_explicit(&probe->done, 1, memory_order_release);
}

static void flowie_client_transport_error(flowie_mqtt_client_t *client, int status, void *user) {
  flowie_client_transport_probe *probe = (flowie_client_transport_probe *)user;
  (void)client;
  (void)status;
  atomic_fetch_add_explicit(&probe->errors, 1, memory_order_relaxed);
  atomic_store_explicit(&probe->done, 1, memory_order_release);
}

static void flowie_client_transport_reconnect_event(
    flowie_mqtt_client_t *client, uint32_t attempt, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_transport_probe *probe = (flowie_client_transport_probe *)user;
  (void)client;
  (void)attempt;
  (void)status;
  (void)response;
  atomic_fetch_add_explicit(&probe->reconnect_attempts, 1, memory_order_relaxed);
}

static cnet_client_config flowie_client_transport_network(void) {
  const cnet_client_config config = {.backend = flowie_client_transport_backend(),
                                     .connection_capacity = 4u,
                                     .command_capacity = 16u,
                                     .request_capacity = 8u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 16u,
                                     .max_send_bytes = 8192u,
                                     .receive_buffer_bytes = 8192u,
                                     .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                                     .tls_handshake_timeout_ms =
                                         FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
                                     .connect_timeout_ms =
                                         FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
                                     .read_timeout_ms =
                                         FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
                                     .write_timeout_ms =
                                         FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS};
  return config;
}

static void flowie_client_transport_case_ex_fixture(
    flowie_mqtt_client_transport_t client_transport, flowie_transport server_transport,
    flowie_client_transport_connack_mode connack_mode, uint64_t client_timeout_ms,
    int expected_connect_status, int with_destination_policy, int round_trips,
    const char *test_ca, const char *test_cert, const char *test_key,
    const char *test_client_ca, const char *test_logical_host,
    int expect_tls_reject) {
  static const unsigned char client_id[] = "cnet-client-test";
  flowie_client_transport_broker broker = {0};
  flowie_client_transport_probe probe = {0};
  flowie_server server = {0};
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  cnet_tls_server_config tls_config = {0};
  flowie_mqtt_client_t *client = NULL;
  char cert_path[512] = {0};
  char key_path[512] = {0};
  char ca_path[512] = {0};
  uint16_t port = 0u;
  uint64_t deadline;

  atomic_init(&broker.opens, 0);
  atomic_init(&broker.connects, 0);
  atomic_init(&broker.publishes, 0);
  atomic_init(&broker.last_publish_qos, 0);
  atomic_init(&broker.pings, 0);
  atomic_init(&broker.disconnects, 0);
  atomic_init(&broker.closes, 0);
  atomic_init(&broker.error, SALTS_OK);
  atomic_init(&probe.done, 0);
  atomic_init(&probe.connect_status, SALTS_EBUSY);
  atomic_init(&probe.ping_status, SALTS_EBUSY);
  atomic_init(&probe.disconnect_status, SALTS_EBUSY);
  atomic_init(&probe.submit_status, SALTS_EBUSY);
  atomic_init(&probe.errors, 0);
  atomic_init(&probe.reconnect_attempts, 0);
  broker.server = &server;
  broker.connack_mode = connack_mode;
  broker.expected_close_status =
      connack_mode == FLOWIE_CLIENT_TRANSPORT_CONNACK_ABRUPT_CLOSE
          ? SALTS_ECONNABORTED
          : SALTS_OK;

  server_config.transport = server_transport;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.stream = flowie_client_transport_network();
  if (server_transport == TF_NET_TRANSPORT_TLS ||
      server_transport == TF_NET_TRANSPORT_WSS) {
    if (test_ca == NULL) {
      check_equal(tls_test_write_server_files(cert_path, sizeof(cert_path),
                                              key_path, sizeof(key_path)), 0);
      check_equal(tls_test_write_ca_file(ca_path, sizeof(ca_path)), 0);
    } else {
      check_not_null(test_cert);
      check_not_null(test_key);
    }
    tls_config = (cnet_tls_server_config){
        .size = sizeof(tls_config),
        .cert_file = test_ca ? test_cert : cert_path,
        .key_file = test_ca ? test_key : key_path,
        .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
    server_config.tls = &tls_config;
    server_config.stream.tls_io_buffer_bytes =
        server_config.stream.receive_buffer_bytes < CNET_TLS_MIN_IO_BUFFER_BYTES
            ? CNET_TLS_MIN_IO_BUFFER_BYTES
            : server_config.stream.receive_buffer_bytes;
    server_config.stream.tls_handshake_timeout_ms =
        FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  }
  server_config.backlog = 4u;
  server_config.path = "/mqtt";
  server_config.websocket_subprotocol =
      (server_transport == TF_NET_TRANSPORT_WS ||
       server_transport == TF_NET_TRANSPORT_WSS) ? "mqtt" : NULL;
  server_config.command_capacity = 8u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.observer = (flowie_observer){flowie_client_transport_open,
                                             flowie_client_transport_receive,
                                             flowie_client_transport_close, NULL, &broker};
  check_equal(flowie_server_init(&server, &server_config), SALTS_OK);
  check_equal(flowie_server_start(&server), SALTS_OK);
  check_equal(flowie_server_port(&server, &port), SALTS_OK);

  client_config.transport = client_transport;
  client_config.host = test_logical_host ? test_logical_host : "127.0.0.1";
  client_config.port = with_destination_policy ? 1 : (int)port;
  client_config.path = "/mqtt";
  client_config.timeout_ms = client_timeout_ms;
  client_config.socket_recv_buffer_bytes = 32768u;
  client_config.socket_send_buffer_bytes = 32768u;
  if (client_transport == FLOWIE_MQTT_CLIENT_TRANSPORT_TLS ||
      client_transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WSS)
    client_config.tls.ca_file = test_ca
        ? (test_client_ca ? test_client_ca : test_ca) : ca_path;
  client_config.on_connect = flowie_client_transport_connect_complete;
  client_config.on_ping = flowie_client_transport_ping_complete;
  client_config.on_disconnect = flowie_client_transport_disconnect_complete;
  client_config.on_error = flowie_client_transport_error;
  client_config.user_data = &probe;
  check_equal(flowie_mqtt_client_create(&client_config, &client), SALTS_OK);
  if (with_destination_policy) {
    char authorized_host[] = "127.0.0.1";
    const flowie_mqtt_client_remote_endpoint_t endpoints[] = {
        {10u, "127.0.0.1", 1u, 1u, 0u, 1u},
        {20u, authorized_host, port, 1u, 0u, 1u}};
    const flowie_mqtt_client_destination_policy_t policy = {
        sizeof(policy), FLOWIE_MQTT_CLIENT_DESTINATION_VERSION,
        FLOWIE_MQTT_DESTINATION_EXPLICIT, endpoints, 2u,
        1u, UINT64_MAX, 0u, 20u, 0u, 0u};
    check_equal(flowie_mqtt_client_set_destination_policy(client, &policy), SALTS_OK);
    strcpy(authorized_host, "invalid"); /* Proves Client owns its dial host. */
  }

  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  for (int round = 0; round < round_trips; ++round) {
    atomic_store_explicit(&probe.done, 0, memory_order_release);
    atomic_store_explicit(&probe.connect_status, SALTS_EBUSY, memory_order_relaxed);
    atomic_store_explicit(&probe.ping_status, SALTS_EBUSY, memory_order_relaxed);
    atomic_store_explicit(&probe.disconnect_status, SALTS_EBUSY, memory_order_relaxed);
    check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);
    if (with_destination_policy) {
      const flowie_mqtt_client_remote_endpoint_t endpoints[] = {
          {20u, "127.0.0.1", port, 1u, 0u, 1u}};
      const flowie_mqtt_client_destination_policy_t policy = {
          sizeof(policy), FLOWIE_MQTT_CLIENT_DESTINATION_VERSION,
          FLOWIE_MQTT_DESTINATION_EXPLICIT, endpoints, 1u,
          1u, UINT64_MAX, 0u, 20u, 0u, 0u};
      check_equal(flowie_mqtt_client_set_destination_policy(client, &policy), SALTS_EBUSY);
    }
    deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&probe.done, memory_order_acquire) &&
           cmeta_monotonic_ms() < deadline)
      cmeta_sleep_ms(1u);

    check_equal(atomic_load_explicit(&probe.done, memory_order_acquire), 1);
    if (expect_tls_reject) {
      /* A bad issuing CA must not be treated as an MQTT CONNACK refusal. */
      check_not_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed),
                      SALTS_OK);
      check_not_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed),
                      SALTS_EBUSY);
      check_equal(flowie_mqtt_client_is_connected(client), 0);
      check_equal(atomic_load_explicit(&broker.opens, memory_order_relaxed), 0);
      check_equal(atomic_load_explicit(&broker.connects, memory_order_relaxed), 0);
      check_equal(atomic_load_explicit(&broker.pings, memory_order_relaxed), 0);
      check_equal(atomic_load_explicit(&probe.reconnect_attempts, memory_order_relaxed), 0);
      break;
    }
    check_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed),
                expected_connect_status);
    if (expected_connect_status == SALTS_OK) {
      check_equal(atomic_load_explicit(&probe.ping_status, memory_order_relaxed), SALTS_OK);
      check_equal(atomic_load_explicit(&probe.disconnect_status, memory_order_relaxed), SALTS_OK);
      check_equal(atomic_load_explicit(&probe.submit_status, memory_order_relaxed), SALTS_OK);
      /* Local command completion does not acknowledge peer processing. Wait
       * for this episode's DISCONNECT before checking counts or reconnecting. */
      while (atomic_load_explicit(&broker.disconnects, memory_order_relaxed) < round + 1 &&
             atomic_load_explicit(&broker.error, memory_order_acquire) == SALTS_OK &&
             cmeta_monotonic_ms() < deadline)
        cmeta_sleep_ms(1u);
    }
    check_equal(atomic_load_explicit(&probe.errors, memory_order_relaxed), 0);
    check_equal(atomic_load_explicit(&broker.opens, memory_order_relaxed), round + 1);
    check_equal(atomic_load_explicit(&broker.connects, memory_order_relaxed), round + 1);
    check_equal(atomic_load_explicit(&broker.pings, memory_order_relaxed),
                expected_connect_status == SALTS_OK ? round + 1 : 0);
    check_equal(atomic_load_explicit(&broker.disconnects, memory_order_relaxed),
                expected_connect_status == SALTS_OK ? round + 1 : 0);
    check_equal(atomic_load_explicit(&broker.error, memory_order_relaxed), SALTS_OK);

  }

  flowie_mqtt_client_destroy(client);
  check_equal(flowie_server_stop(&server, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(flowie_server_destroy(&server), SALTS_OK);
  if (test_ca == NULL) {
    tls_test_remove_file(ca_path);
    tls_test_remove_file(key_path);
    tls_test_remove_file(cert_path);
  }
}

static void flowie_client_transport_case_ex(
    flowie_mqtt_client_transport_t client_transport, flowie_transport server_transport,
    flowie_client_transport_connack_mode connack_mode, uint64_t client_timeout_ms,
    int expected_connect_status, int with_destination_policy, int round_trips) {
  flowie_client_transport_case_ex_fixture(client_transport, server_transport,
      connack_mode, client_timeout_ms, expected_connect_status,
      with_destination_policy, round_trips, NULL, NULL, NULL, NULL, NULL, 0);
}

static void flowie_client_transport_case(flowie_mqtt_client_transport_t client_transport,
                                         flowie_transport server_transport,
                                         flowie_client_transport_connack_mode connack_mode,
                                         uint64_t client_timeout_ms, int expected_connect_status) {
  flowie_client_transport_case_ex(client_transport, server_transport, connack_mode,
                                  client_timeout_ms, expected_connect_status, 0, 1);
}

static void flowie_client_transport_abrupt_tls_close(void) {
  static const unsigned char client_id[] = "cnet-client-tls-eof";
  flow_mtls_test_server_t server;
  flowie_client_transport_probe probe = {0};
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_t *client = NULL;
  char ca_path[512] = {0};
  uint64_t deadline;
  int setup_status;

  atomic_init(&probe.done, 0);
  atomic_init(&probe.connect_status, SALTS_EBUSY);
  atomic_init(&probe.ping_status, SALTS_EBUSY);
  atomic_init(&probe.disconnect_status, SALTS_EBUSY);
  atomic_init(&probe.submit_status, SALTS_EBUSY);
  atomic_init(&probe.errors, 0);
  atomic_init(&probe.reconnect_attempts, 0);

  check_equal(tls_test_write_ca_file(ca_path, sizeof(ca_path)), 0);
  setup_status = flow_tls_test_server_start_abrupt(&server);
  if (setup_status != 0) {
    tls_test_remove_file(ca_path);
    check_equal(setup_status, 0);
    return;
  }

  client_config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
  client_config.host = "localhost";
  client_config.port = (int)server.port;
  client_config.timeout_ms = FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  client_config.socket_recv_buffer_bytes = 32768u;
  client_config.socket_send_buffer_bytes = 32768u;
  client_config.tls.ca_file = ca_path;
  client_config.on_connect = flowie_client_transport_connect_complete;
  client_config.on_ping = flowie_client_transport_ping_complete;
  client_config.on_disconnect = flowie_client_transport_disconnect_complete;
  client_config.on_error = flowie_client_transport_error;
  client_config.user_data = &probe;
  setup_status = flowie_mqtt_client_create(&client_config, &client);
  if (setup_status != SALTS_OK) goto cleanup;

  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  setup_status = flowie_mqtt_client_connect(client, &connect);
  if (setup_status != SALTS_OK) goto cleanup;
  deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(&probe.done, memory_order_acquire) &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1u);

cleanup:
  /* Join both workers before a fatal assertion can unwind their borrowed
   * stack state. Keep the peer socket alive until the client is finished. */
  flowie_mqtt_client_destroy(client);
  flow_mtls_test_server_join(&server);
  tls_test_remove_file(ca_path);
  info("TLS EOF peer: status=%d, request_size=%zu, eof_sent=%d, connect_status=%d",
       server.status, server.request_size, server.eof_sent,
       atomic_load_explicit(&probe.connect_status, memory_order_relaxed));
  check_equal(setup_status, SALTS_OK);
  check_equal(server.status, 0);
  check_greater(server.request_size, (size_t)0u);
  check_equal(server.request[0] >> 4u, FLOWIE_MQTT_PACKET_CONNECT);
  check_equal(server.eof_sent, 1);
  check_equal(atomic_load_explicit(&probe.done, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed),
              SALTS_ECONNABORTED);
  check_equal(atomic_load_explicit(&probe.errors, memory_order_relaxed), 0);
}


static void flowie_client_transport_tls_identity_fail_closed(void) {
  static const unsigned char client_id[] = "cnet-client-tls-identity-rejection";
  flow_mtls_test_server_t server;
  flowie_client_transport_probe probe = {0};
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_client_resilience_config_t resilience = FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_t *client = NULL;
  char ca_path[512] = {0};
  uint64_t deadline;
  atomic_init(&probe.done, 0);
  atomic_init(&probe.connect_status, SALTS_EBUSY);
  atomic_init(&probe.ping_status, SALTS_EBUSY);
  atomic_init(&probe.disconnect_status, SALTS_EBUSY);
  atomic_init(&probe.submit_status, SALTS_EBUSY);
  atomic_init(&probe.errors, 0);
  atomic_init(&probe.reconnect_attempts, 0);
  check_equal(tls_test_write_ca_file(ca_path, sizeof(ca_path)), 0);
  check_equal(flow_tls_test_server_start_abrupt(&server), 0);

  client_config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
  client_config.host = "wrong-tls-identity.invalid";
  client_config.port = 1; /* Must never be dialed: CNet selects authorized target. */
  client_config.timeout_ms = FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  client_config.tls.ca_file = ca_path;
  client_config.on_connect = flowie_client_transport_connect_complete;
  client_config.on_ping = flowie_client_transport_ping_complete;
  client_config.on_disconnect = flowie_client_transport_disconnect_complete;
  client_config.on_error = flowie_client_transport_error;
  client_config.user_data = &probe;
  resilience.initial_delay_ms = 25u;
  resilience.max_delay_ms = 90u;
  resilience.max_attempts = 0u; /* Legacy unlimited retries must NOT bypass security. */
  resilience.on_reconnect = flowie_client_transport_reconnect_event;
  check_equal(flowie_mqtt_client_create_ex(&client_config, &resilience, &client), SALTS_OK);
  {
    const flowie_mqtt_client_remote_endpoint_t endpoints[] = {
        {100u, "127.0.0.1", server.port, 1u, 0u, 1u}};
    const flowie_mqtt_client_destination_policy_t destination = {
        sizeof(destination), FLOWIE_MQTT_CLIENT_DESTINATION_VERSION,
        FLOWIE_MQTT_DESTINATION_EXPLICIT, endpoints, 1u,
        1u, UINT64_MAX, 0u, 100u, 0u, 0u};
    check_equal(flowie_mqtt_client_set_destination_policy(client, &destination), SALTS_OK);
  }

  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);
  deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(&probe.done, memory_order_acquire) &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1u);
  check_equal(atomic_load_explicit(&probe.done, memory_order_acquire), 1);
  check_not_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed), SALTS_OK);
  check_not_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed), SALTS_EBUSY);
  check_equal(flowie_mqtt_client_is_connected(client), 0);
  cmeta_sleep_ms(200u); /* Longer than the configured retry delay. */
  check_equal(atomic_load_explicit(&probe.reconnect_attempts, memory_order_acquire), 0);
  flowie_mqtt_client_destroy(client);
  flow_mtls_test_server_join(&server);
  /* A wrong TLS authority must never progress to MQTT CONNECT. */
  check_equal(server.request_size, (size_t)0u);
  tls_test_remove_file(ca_path);
}

static void flowie_client_transport_destroy_pending_connack(void) {
  static const unsigned char id[] = "managed-dial-pending-destroy";
  flowie_client_transport_broker broker = {0};
  flowie_client_transport_probe probe = {0};
  flowie_server server = {0};
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_t *client = NULL;
  uint16_t port = 0u;
  uint64_t deadline;
  atomic_init(&broker.opens, 0);
  atomic_init(&broker.connects, 0);
  atomic_init(&broker.publishes, 0);
  atomic_init(&broker.last_publish_qos, 0);
  atomic_init(&broker.pings, 0);
  atomic_init(&broker.disconnects, 0);
  atomic_init(&broker.closes, 0);
  atomic_init(&broker.error, SALTS_OK);
  atomic_init(&probe.done, 0);
  atomic_init(&probe.connect_status, SALTS_EBUSY);
  atomic_init(&probe.ping_status, SALTS_EBUSY);
  atomic_init(&probe.disconnect_status, SALTS_EBUSY);
  atomic_init(&probe.submit_status, SALTS_EBUSY);
  atomic_init(&probe.errors, 0);
  atomic_init(&probe.reconnect_attempts, 0);
  broker.server = &server;
  broker.connack_mode = FLOWIE_CLIENT_TRANSPORT_CONNACK_SILENT;
  server_config.transport = TF_NET_TRANSPORT_TCP;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.backlog = 4u;
  server_config.stream = flowie_client_transport_network();
  server_config.command_capacity = 8u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.observer = (flowie_observer){
      flowie_client_transport_open, flowie_client_transport_receive,
      flowie_client_transport_close, NULL, &broker};
  check_equal(flowie_server_init(&server, &server_config), SALTS_OK);
  check_equal(flowie_server_start(&server), SALTS_OK);
  check_equal(flowie_server_port(&server, &port), SALTS_OK);

  client_config.host = "127.0.0.1";
  client_config.port = (int)port;
  client_config.timeout_ms = FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  client_config.on_connect = flowie_client_transport_connect_complete;
  client_config.on_ping = flowie_client_transport_ping_complete;
  client_config.on_disconnect = flowie_client_transport_disconnect_complete;
  client_config.on_error = flowie_client_transport_error;
  client_config.user_data = &probe;
  check_equal(flowie_mqtt_client_create(&client_config, &client), SALTS_OK);
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){id, sizeof(id) - 1u};
  check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);
  deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(&broker.connects, memory_order_acquire) == 0 &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1u);
  check_equal(atomic_load_explicit(&broker.connects, memory_order_acquire), 1);
  /* The backend is alive and waiting on CONNACK. A zero-budget call must
   * preserve ownership, not free a user pointer still in its observer. */
  check_equal(flowie_mqtt_client_try_destroy(client, 0u), SALTS_EBUSY);
  check_equal(flowie_mqtt_client_try_destroy(
      client, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(atomic_load_explicit(&broker.pings, memory_order_acquire), 0);
  check_equal(atomic_load_explicit(&broker.error, memory_order_acquire), SALTS_OK);
  check_equal(flowie_server_stop(&server, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(flowie_server_destroy(&server), SALTS_OK);
}

/* Test the self-stop request from a real authenticated MQTT callback,
 * then finish teardown externally on the very same native Owner. Both raw
 * CNet TCP and CHttp WebSocket must retain their borrowed observer until drain. */
static void flowie_client_transport_self_stop_case(
    flowie_mqtt_client_transport_t client_transport, flowie_transport server_transport) {
  static const unsigned char client_id[] = "flowie-callback-owner-stop";
  flowie_client_transport_broker broker = {0};
  flowie_client_transport_probe probe = {0};
  flowie_server server = {0};
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_t *client = NULL;
  uint16_t port = 0u;
  uint64_t deadline;

  atomic_init(&broker.opens, 0);
  atomic_init(&broker.connects, 0);
  atomic_init(&broker.publishes, 0);
  atomic_init(&broker.last_publish_qos, 0);
  atomic_init(&broker.pings, 0);
  atomic_init(&broker.disconnects, 0);
  atomic_init(&broker.closes, 0);
  atomic_init(&broker.error, SALTS_OK);
  atomic_init(&probe.done, 0);
  atomic_init(&probe.connect_status, SALTS_EBUSY);
  atomic_init(&probe.ping_status, SALTS_EBUSY);
  atomic_init(&probe.disconnect_status, SALTS_EBUSY);
  atomic_init(&probe.submit_status, SALTS_EBUSY);
  atomic_init(&probe.errors, 0);
  atomic_init(&probe.reconnect_attempts, 0);
  atomic_init(&probe.callback_destroy_status, SALTS_EINVAL);
  broker.server = &server;
  broker.connack_mode = FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID;
  server_config.transport = server_transport;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.backlog = 4u;
  server_config.path = "/mqtt";
  server_config.websocket_subprotocol =
      server_transport == TF_NET_TRANSPORT_WS ? "mqtt" : NULL;
  server_config.stream = flowie_client_transport_network();
  server_config.command_capacity = 8u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.observer = (flowie_observer){
      flowie_client_transport_open, flowie_client_transport_receive,
      flowie_client_transport_close, NULL, &broker};
  check_equal(flowie_server_init(&server, &server_config), SALTS_OK);
  check_equal(flowie_server_start(&server), SALTS_OK);
  check_equal(flowie_server_port(&server, &port), SALTS_OK);

  client_config.transport = client_transport;
  client_config.host = "127.0.0.1";
  client_config.port = (int)port;
  client_config.path = "/mqtt";
  client_config.timeout_ms = FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  client_config.on_connect = flowie_client_transport_connect_self_stop;
  client_config.on_ping = flowie_client_transport_ping_complete;
  client_config.on_error = flowie_client_transport_error;
  client_config.user_data = &probe;
  check_equal(flowie_mqtt_client_create(&client_config, &client), SALTS_OK);
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);

  deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(&probe.done, memory_order_acquire) == 0 &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1u);
  check_equal(atomic_load_explicit(&probe.done, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed), SALTS_OK);
  check_equal(atomic_load_explicit(&probe.callback_destroy_status, memory_order_relaxed),
              SALTS_EBUSY);
  /* Admission is sealed by the in-Worker stop request, not deferred to
   * external cleanup. No PING may slip through the callback boundary. */
  check_equal(atomic_load_explicit(&probe.submit_status, memory_order_relaxed),
              SALTS_ESHUTDOWN);
  check_equal(flowie_mqtt_client_try_destroy(
                  client, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(atomic_load_explicit(&broker.opens, memory_order_relaxed), 1);
  check_equal(atomic_load_explicit(&broker.connects, memory_order_relaxed), 1);
  check_equal(atomic_load_explicit(&broker.pings, memory_order_relaxed), 0);
  check_equal(atomic_load_explicit(&broker.error, memory_order_relaxed), SALTS_OK);
  check_equal(flowie_server_stop(&server, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(flowie_server_destroy(&server), SALTS_OK);
}

typedef struct flowie_client_transport_qos_probe {
  atomic_int submit_status;
  atomic_int publish_completions;
  atomic_int publish_completion_status;
  uint8_t qos;
} flowie_client_transport_qos_probe;

static void flowie_client_transport_qos_connect(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_transport_qos_probe *probe = (flowie_client_transport_qos_probe *)user;
  static const unsigned char topic_name[] = "flowie/lifetime/inflight";
  static const unsigned char payload[] = "publish must not replay on shutdown";
  flowie_mqtt_client_publish_topic_t topic = {0};
  flowie_mqtt_client_publish_topic_vec_t topics = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
  if (status == SALTS_OK && response != NULL &&
      response->type == FLOWIE_MQTT_PACKET_CONNACK && response->reason_code == 0u) {
    topic.qos = probe->qos;
    topic.topic = (flowie_mqtt_span_t){topic_name, sizeof(topic_name) - 1u};
    topic.payload = (flowie_mqtt_span_t){payload, sizeof(payload) - 1u};
    topics.version = FLOWIE_MQTT_VERSION_5;
    topics.data = &topic;
    topics.count = 1u;
    status = flowie_mqtt_client_publish(client, &topics);
  } else if (status == SALTS_OK) {
    status = SALTS_EPROTO;
  }
  atomic_store_explicit(&probe->submit_status, status, memory_order_release);
}

static void flowie_client_transport_qos_publish_done(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_transport_qos_probe *probe = (flowie_client_transport_qos_probe *)user;
  (void)client;
  (void)response;
  atomic_store_explicit(&probe->publish_completion_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&probe->publish_completions, 1, memory_order_release);
}

/* The broker receives exactly one QoS1/2 PUBLISH but intentionally never
 * completes its ack handshake. Shutdown must cancel without replay, and must
 * retain the packet buffers/owner until the real CNet drain completes. */
static void flowie_client_transport_qos_shutdown_case(uint8_t qos) {
  static const unsigned char id[] = "qos-inflight-teardown";
  static const unsigned char topic_name[] = "flowie/lifetime/inflight";
  static const unsigned char payload[] = "another publish after stop";
  flowie_client_transport_broker broker = {0};
  flowie_client_transport_qos_probe probe = {0};
  flowie_server server = {0};
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_publish_topic_t topic = {0};
  flowie_mqtt_client_publish_topic_vec_t topics = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
  flowie_mqtt_client_t *client = NULL;
  uint64_t deadline;
  uint16_t port = 0u;
  atomic_init(&broker.opens, 0);
  atomic_init(&broker.connects, 0);
  atomic_init(&broker.publishes, 0);
  atomic_init(&broker.last_publish_qos, 0);
  atomic_init(&broker.pings, 0);
  atomic_init(&broker.disconnects, 0);
  atomic_init(&broker.closes, 0);
  atomic_init(&broker.error, SALTS_OK);
  atomic_init(&probe.submit_status, SALTS_EBUSY);
  atomic_init(&probe.publish_completions, 0);
  atomic_init(&probe.publish_completion_status, SALTS_EBUSY);
  probe.qos = qos;
  broker.server = &server;
  broker.connack_mode = FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID;

  server_config.transport = TF_NET_TRANSPORT_TCP;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.backlog = 4u;
  server_config.stream = flowie_client_transport_network();
  server_config.command_capacity = 8u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.observer = (flowie_observer){
      flowie_client_transport_open, flowie_client_transport_receive,
      flowie_client_transport_close, NULL, &broker};
  check_equal(flowie_server_init(&server, &server_config), SALTS_OK);
  check_equal(flowie_server_start(&server), SALTS_OK);
  check_equal(flowie_server_port(&server, &port), SALTS_OK);

  client_config.host = "127.0.0.1";
  client_config.port = (int)port;
  client_config.timeout_ms = FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  client_config.on_connect = flowie_client_transport_qos_connect;
  client_config.on_publish = flowie_client_transport_qos_publish_done;
  client_config.user_data = &probe;
  check_equal(flowie_mqtt_client_create(&client_config, &client), SALTS_OK);
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){id, sizeof(id) - 1u};
  check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);

  deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(&broker.publishes, memory_order_acquire) == 0 &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1u);
  check_equal(atomic_load_explicit(&probe.submit_status, memory_order_acquire), SALTS_OK);
  check_equal(atomic_load_explicit(&broker.publishes, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&broker.last_publish_qos, memory_order_acquire), qos);
  check_equal(atomic_load_explicit(&probe.publish_completions, memory_order_acquire), 0);

  check_equal(flowie_mqtt_client_try_destroy(client, 0u), SALTS_EBUSY);
  topic.qos = qos;
  topic.topic = (flowie_mqtt_span_t){topic_name, sizeof(topic_name) - 1u};
  topic.payload = (flowie_mqtt_span_t){payload, sizeof(payload) - 1u};
  topics.version = FLOWIE_MQTT_VERSION_5;
  topics.data = &topic;
  topics.count = 1u;
  check_equal(flowie_mqtt_client_publish(client, &topics), SALTS_ESHUTDOWN);
  check_equal(flowie_mqtt_client_try_destroy(
      client, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(atomic_load_explicit(&probe.publish_completions, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&probe.publish_completion_status, memory_order_acquire),
              SALTS_ESHUTDOWN);
  check_equal(atomic_load_explicit(&broker.publishes, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&broker.error, memory_order_acquire), SALTS_OK);
  check_equal(flowie_server_stop(&server, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(flowie_server_destroy(&server), SALTS_OK);
}

#if defined(FLOWIE_CLIENT_FAULT_TEST)
static void flowie_client_transport_fault_connect_complete(
    flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  atomic_int *callbacks = (atomic_int *)user;
  (void)client;
  (void)status;
  (void)response;
  atomic_fetch_add_explicit(callbacks, 1, memory_order_relaxed);
}

/* Connect a real TCP broker but withhold CONNACK. Force the Managed Dial
 * seal() CLOSE admission FULL, prove the original Owner makes progress and
 * a bounded failure never frees the still-borrowed Client. */
static void flowie_client_transport_injected_full_close(void) {
  static const unsigned char id[] = "dial-seal-full-lifetime";
  flowie_client_transport_broker broker = {0};
  flowie_server server = {0};
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_t *client = NULL;
  atomic_int connect_callbacks;
  uint16_t port = 0u;
  uint64_t deadline;
  int status;

  atomic_init(&connect_callbacks, 0);
  atomic_init(&broker.opens, 0);
  atomic_init(&broker.connects, 0);
  atomic_init(&broker.publishes, 0);
  atomic_init(&broker.last_publish_qos, 0);
  atomic_init(&broker.pings, 0);
  atomic_init(&broker.disconnects, 0);
  atomic_init(&broker.closes, 0);
  atomic_init(&broker.error, SALTS_OK);
  broker.server = &server;
  broker.connack_mode = FLOWIE_CLIENT_TRANSPORT_CONNACK_SILENT;

  server_config.transport = TF_NET_TRANSPORT_TCP;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.backlog = 4u;
  server_config.stream = flowie_client_transport_network();
  server_config.command_capacity = 8u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.observer = (flowie_observer){
      flowie_client_transport_open, flowie_client_transport_receive,
      flowie_client_transport_close, NULL, &broker};
  check_equal(flowie_server_init(&server, &server_config), SALTS_OK);
  check_equal(flowie_server_start(&server), SALTS_OK);
  check_equal(flowie_server_port(&server, &port), SALTS_OK);

  config.host = "127.0.0.1";
  config.port = (int)port;
  config.timeout_ms = FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  config.on_connect = flowie_client_transport_fault_connect_complete;
  config.on_ping = flowie_client_transport_fault_connect_complete;
  config.user_data = &connect_callbacks;
  check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){id, sizeof(id) - 1u};
  check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);
  deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(&broker.connects, memory_order_acquire) == 0 &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1u);
  check_equal(atomic_load_explicit(&broker.connects, memory_order_acquire), 1);

  check_equal(flowie_mqtt_client_test_force_close_full(client, 1), SALTS_OK);
  status = flowie_mqtt_client_try_destroy(client, 150u);
  check(status == SALTS_ETIMEDOUT || status == SALTS_EBUSY || status == SALTS_ENOBUFS);
  check(flowie_mqtt_client_test_close_full_hits(client) > 0u);
  check(flowie_mqtt_client_test_close_full_progress(client) > 0u);
  check_equal(flowie_mqtt_client_test_close_full_wrong_owner(client), 0u);
  /* The first call reported incomplete teardown: Client and borrowed Owner
   * are still live, but new commands are rejected immediately. */
  check_equal(flowie_mqtt_client_ping(client), SALTS_ESHUTDOWN);
  check_equal(flowie_mqtt_client_test_force_close_full(client, 0), SALTS_OK);
  check_equal(flowie_mqtt_client_try_destroy(
      client, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(atomic_load_explicit(&connect_callbacks, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&broker.pings, memory_order_acquire), 0);
  check_equal(atomic_load_explicit(&broker.error, memory_order_acquire), SALTS_OK);
  check_equal(flowie_server_stop(&server, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(flowie_server_destroy(&server), SALTS_OK);
}
/* Real loopback WSS or WS transport with pending MQTT CONNACK. The
 * isolated test DLL refuses CHttp destruction, forcing try_destroy() to
 * retain its TLS/backend/Client borrows until a same-Owner retry. */
static void flowie_client_transport_ws_destroy_timeout_case(int use_tls) {
  static const unsigned char id[] = "ws-native-close-timeout";
  flowie_client_transport_broker broker = {0};
  atomic_int connect_callbacks;
  flowie_server server = {0};
  flowie_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_t *client = NULL;
  cnet_tls_server_config tls = {0};
  char ca_file[512] = {0};
  char cert_file[512] = {0};
  char key_file[512] = {0};
  uint64_t deadline;
  uint16_t port = 0u;
  int status;

  atomic_init(&connect_callbacks, 0);
  atomic_init(&broker.opens, 0);
  atomic_init(&broker.connects, 0);
  atomic_init(&broker.publishes, 0);
  atomic_init(&broker.last_publish_qos, 0);
  atomic_init(&broker.pings, 0);
  atomic_init(&broker.disconnects, 0);
  atomic_init(&broker.closes, 0);
  atomic_init(&broker.error, SALTS_OK);
  broker.server = &server;
  broker.connack_mode = FLOWIE_CLIENT_TRANSPORT_CONNACK_SILENT;

  if (use_tls) {
    check_equal(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                            key_file, sizeof(key_file)), 0);
    check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    tls.size = sizeof(tls);
    tls.cert_file = cert_file;
    tls.key_file = key_file;
    tls.client_auth = CNET_TLS_CLIENT_AUTH_NONE;
  }
  server_config.transport = use_tls ? TF_NET_TRANSPORT_WSS : TF_NET_TRANSPORT_WS;
  server_config.host = "127.0.0.1";
  server_config.port = 0u;
  server_config.backlog = 4u;
  server_config.path = "/mqtt";
  server_config.websocket_subprotocol = "mqtt";
  server_config.stream = flowie_client_transport_network();
  server_config.command_capacity = 8u;
  server_config.command_bytes_capacity = 8192u;
  server_config.max_message_bytes = FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES;
  server_config.poll_slice_ms = 1u;
  server_config.tls = use_tls ? &tls : NULL;
  server_config.observer = (flowie_observer){
      flowie_client_transport_open, flowie_client_transport_receive,
      flowie_client_transport_close, NULL, &broker};
  check_equal(flowie_server_init(&server, &server_config), SALTS_OK);
  check_equal(flowie_server_start(&server), SALTS_OK);
  check_equal(flowie_server_port(&server, &port), SALTS_OK);

  config.transport = use_tls ? FLOWIE_MQTT_CLIENT_TRANSPORT_WSS : FLOWIE_MQTT_CLIENT_TRANSPORT_WS;
  config.host = "127.0.0.1";
  config.port = (int)port;
  config.path = "/mqtt";
  config.timeout_ms = FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  config.tls.ca_file = use_tls ? ca_file : NULL;
  config.on_connect = flowie_client_transport_fault_connect_complete;
  config.on_ping = flowie_client_transport_fault_connect_complete;
  config.user_data = &connect_callbacks;
  check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
  check_equal(flowie_mqtt_client_test_set_ws_destroy_timeout(client, 1), SALTS_OK);
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){id, sizeof(id) - 1u};
  check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);

  deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(&broker.connects, memory_order_acquire) == 0 &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1u);
  check_equal(atomic_load_explicit(&broker.connects, memory_order_acquire), 1);

  status = flowie_mqtt_client_try_destroy(client, 250u);
  check(status == SALTS_ETIMEDOUT || status == SALTS_EBUSY);
  check(flowie_mqtt_client_test_ws_destroy_timeout_hits(client) > 0u);
  check_equal(flowie_mqtt_client_ping(client), SALTS_ESHUTDOWN);
  check_equal(flowie_mqtt_client_test_set_ws_destroy_timeout(client, 0), SALTS_OK);
  check_equal(flowie_mqtt_client_try_destroy(
      client, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(atomic_load_explicit(&connect_callbacks, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&broker.pings, memory_order_acquire), 0);
  check_equal(atomic_load_explicit(&broker.error, memory_order_acquire), SALTS_OK);
  check_equal(flowie_server_stop(&server, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(flowie_server_destroy(&server), SALTS_OK);
  if (use_tls) {
    tls_test_remove_file(ca_file);
    tls_test_remove_file(cert_file);
    tls_test_remove_file(key_file);
  }
}

#endif

#if defined(FLOWIE_CLIENT_FAULT_TEST)
typedef struct flowie_client_dial_admission_probe {
  atomic_int done;
  const char *expected_ca;
  int expect_tls;
  int inspected;
  int physical_target_matches;
  int tls_matches;
  int status;
} flowie_client_dial_admission_probe;

static int flowie_client_inspect_dial(const cnet_connect_options *options, void *context) {
  flowie_client_dial_admission_probe *probe = context;
  probe->inspected++;
  probe->physical_target_matches = strcmp(options->uri, probe->expect_tls
      ? "tls://127.0.0.1:1883" : "tcp://127.0.0.1:1883") == 0;
  if (!probe->expect_tls) probe->tls_matches = options->tls == NULL;
  else if (options->tls) {
    const cnet_tls_client_config *tls = options->tls;
    probe->tls_matches = tls->size == sizeof(*tls) && tls->server_name &&
        strcmp(tls->server_name, "broker.example") == 0 &&
        ((probe->expected_ca && tls->ca_file &&
          strcmp(tls->ca_file, probe->expected_ca) == 0) ||
         (!probe->expected_ca && !tls->ca_file)) &&
        !tls->cert_file && !tls->key_file && !tls->key_password;
  }
  /* Admission contract only: no trust-store mutation or remote connection. */
  return SALTS_EINVAL;
}

static void flowie_client_dial_inspected(flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *context) {
  flowie_client_dial_admission_probe *probe = context;
  (void)client;
  (void)response;
  probe->status = status;
  atomic_store_explicit(&probe->done, 1, memory_order_release);
}

spec("Flowie Client logical TLS authority admission") {
  static flowie_mqtt_client_t *client;
  static flowie_client_dial_admission_probe probe;
  before_each() {
    client = NULL;
    memset(&probe, 0, sizeof(probe));
    atomic_init(&probe.done, 0);
  }
  after_each() { flowie_mqtt_client_destroy(client); }

  static const struct { flowie_mqtt_client_transport_t transport; const char *ca; } cases[] = {
      {FLOWIE_MQTT_CLIENT_TRANSPORT_TLS, NULL},
      {FLOWIE_MQTT_CLIENT_TRANSPORT_TLS, "configured-ca.pem"},
      {FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, NULL}};
  for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    it("keeps logical identity separate from physical destination, case %zu", index) {
      flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
      flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
      const flowie_mqtt_client_remote_endpoint_t destination =
          {10u, "127.0.0.1", 1883u, 1u, 0u, 1u};
      const flowie_mqtt_client_destination_policy_t policy = {
          sizeof(policy), FLOWIE_MQTT_CLIENT_DESTINATION_VERSION,
          FLOWIE_MQTT_DESTINATION_EXPLICIT, &destination, 1u,
          1u, UINT64_MAX, 0u, 10u, 0u, 0u};
      probe.expect_tls = cases[index].transport == FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
      probe.expected_ca = cases[index].ca;
      config.transport = cases[index].transport;
      config.host = "broker.example";
      config.port = 1883;
      config.tls.ca_file = cases[index].ca;
      config.on_connect = flowie_client_dial_inspected;
      config.user_data = &probe;
      check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
      check_equal(flowie_mqtt_client_set_destination_policy(client, &policy), SALTS_OK);
      check_equal(flowie_mqtt_client_test_set_dial_admission(client,
          flowie_client_inspect_dial, &probe), SALTS_OK);
      connect.version = FLOWIE_MQTT_VERSION_5;
      connect.clean_start = 1u;
      connect.client_id = (flowie_mqtt_span_t){(const uint8_t *)"tls-admission", 13u};
      check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);
      const uint64_t deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
      while (!atomic_load_explicit(&probe.done, memory_order_acquire) &&
             cmeta_monotonic_ms() < deadline) cmeta_sleep_ms(1u);
      check_equal(atomic_load_explicit(&probe.done, memory_order_acquire), 1);
      check_equal(probe.inspected, 1);
      check_equal(probe.status, SALTS_EINVAL);
      check_true(probe.physical_target_matches);
      check_true(probe.tls_matches);
    }
  }
}
#endif

#if defined(FLOWIE_CLIENT_FAULT_TEST)
enum {
  FLOWIE_CONTRACT_CONNECT,
  FLOWIE_CONTRACT_PUBLISH,
  FLOWIE_CONTRACT_RECONNECT,
  FLOWIE_CONTRACT_CALLBACK_COUNT,
  FLOWIE_CONTRACT_COMMAND_MS = 300u,
  FLOWIE_CONTRACT_STAGE_MS = 180u
};

typedef struct flowie_client_contract_fixture {
  flowie_server server;
  flowie_mqtt_client_t *client;
  int server_initialized;
  int server_started;
  int refresh;
  int resilience_enabled;
  int disconnect_first_publish;
  int probe_reason;
  uint32_t ack_delay_ms;
  unsigned char input[FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES];
  size_t input_size;
  unsigned char password[16];
  atomic_int completions[FLOWIE_CONTRACT_CALLBACK_COUNT];
  atomic_int statuses[FLOWIE_CONTRACT_CALLBACK_COUNT];
  atomic_int connects;
  atomic_int publishes;
  atomic_int pubrels;
  atomic_int bad_passwords;
  atomic_int closed_connections;
  atomic_int errors;
  atomic_int refreshes;
  atomic_int reason_callback_entered;
  atomic_int reason_callback_release;
  atomic_int callback_reason_status;
  atomic_int callback_reason;
} flowie_client_contract_fixture;

static void flowie_client_contract_complete(flowie_client_contract_fixture *f, int kind,
                                             int status) {
  atomic_store_explicit(&f->statuses[kind], status, memory_order_relaxed);
  atomic_fetch_add_explicit(&f->completions[kind], 1, memory_order_release);
}

static void flowie_client_contract_connected(flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  (void)client;
  (void)response;
  flowie_client_contract_complete(user, FLOWIE_CONTRACT_CONNECT, status);
}

static void flowie_client_contract_published(flowie_mqtt_client_t *client, int status,
    const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_contract_fixture *f = user;
  (void)response;
  if (f->probe_reason) {
    uint8_t reason = 0u;
    const int reason_status = flowie_mqtt_client_server_disconnect_reason(client, &reason);
    atomic_store_explicit(&f->callback_reason_status, reason_status, memory_order_relaxed);
    atomic_store_explicit(&f->callback_reason, reason, memory_order_relaxed);
    atomic_store_explicit(&f->reason_callback_entered, 1, memory_order_release);
    /* A bounded test barrier exposes the real callback's thread boundary. */
    const uint64_t deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
    while (!atomic_load_explicit(&f->reason_callback_release, memory_order_acquire) &&
           cmeta_monotonic_ms() < deadline) cmeta_sleep_ms(1u);
  }
  flowie_client_contract_complete(f, FLOWIE_CONTRACT_PUBLISH, status);
}

static void flowie_client_contract_error(flowie_mqtt_client_t *client, int status, void *user) {
  flowie_client_contract_fixture *f = user;
  (void)client;
  (void)status;
  atomic_fetch_add_explicit(&f->errors, 1, memory_order_relaxed);
}

static int flowie_client_contract_refresh(flowie_mqtt_client_t *client, uint8_t reason,
    const flowie_mqtt_connect_packet_t *current, flowie_mqtt_connect_packet_t *out, void *user) {
  flowie_client_contract_fixture *f = user;
  (void)client;
  if (reason != 0x87u) return SALTS_EPROTO;
  *out = *current; /* The current packet's other spans are valid borrowed input. */
  memcpy(f->password, "refreshed", 9u);
  out->password = (flowie_mqtt_span_t){f->password, 9u};
  atomic_fetch_add_explicit(&f->refreshes, 1, memory_order_relaxed);
  return SALTS_OK;
}

static void flowie_client_contract_reconnected(flowie_mqtt_client_t *client, uint32_t attempt,
    int status, const flowie_mqtt_control_packet_view_t *response, void *user) {
  flowie_client_contract_fixture *f = user;
  (void)client;
  /* The next callback ends the refresh output's borrow; retained CONNECT must
   * already own a copy for the subsequent ordinary Server Busy retry. */
  if (attempt == 1u) memset(f->password, 'x', sizeof(f->password));
  if (status == SALTS_OK && (!response || response->reason_code != 0u))
    status = SALTS_ECONNREFUSED;
  flowie_client_contract_complete(f, FLOWIE_CONTRACT_RECONNECT, status);
}

static int flowie_client_contract_open(void *user, flowie_connection connection,
                                       const flowie_peer_info *peer) {
  flowie_client_contract_fixture *f = user;
  (void)connection;
  (void)peer;
  f->input_size = 0u;
  return SALTS_OK;
}

static int flowie_client_contract_receive(void *user, flowie_connection connection,
                                          const void *data, size_t size) {
  flowie_client_contract_fixture *f = user;
  flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  if (size > sizeof(f->input) - f->input_size) return SALTS_EMSGSIZE;
  memcpy(f->input + f->input_size, data, size);
  f->input_size += size;
  options.version = FLOWIE_MQTT_VERSION_5;
  while (f->input_size != 0u) {
    flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
    flowie_mqtt_control_packet_t reply = FLOWIE_MQTT_CONTROL_PACKET_INIT;
    size_t consumed = 0u, written = 0u;
    unsigned char output[32];
    int status = flowie_mqtt_packet_parse(f->input, f->input_size, &options,
                                          &packet, &consumed, NULL);
    if (status == FLOWIE_MQTT_PARSE_NEED_MORE) return SALTS_OK;
    if (status != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
    reply.version = FLOWIE_MQTT_VERSION_5;
    if (packet.type == FLOWIE_MQTT_PACKET_CONNECT) {
      flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
      int episode = atomic_fetch_add_explicit(&f->connects, 1, memory_order_relaxed) + 1;
      if (flowie_mqtt_connect_parse(&packet, &connect) != FLOWIE_MQTT_PARSE_OK)
        return SALTS_EPROTO;
      const char *password = f->refresh && episode >= 2 ? "refreshed" : "initial";
      const size_t password_size = strlen(password);
      if (connect.password.size != password_size ||
          memcmp(connect.password.data, password, password_size))
        atomic_fetch_add_explicit(&f->bad_passwords, 1, memory_order_relaxed);
      reply.type = FLOWIE_MQTT_PACKET_CONNACK;
      reply.reason_code = !f->refresh || episode >= 3 ? 0u : episode == 1 ? 0x87u : 0x89u;
    } else if (packet.type == FLOWIE_MQTT_PACKET_PUBLISH) {
      flowie_mqtt_publish_view_t publish = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
      if (flowie_mqtt_publish_parse(&packet, &publish) != FLOWIE_MQTT_PARSE_OK)
        return SALTS_EPROTO;
      atomic_fetch_add_explicit(&f->publishes, 1, memory_order_release);
      if (publish.qos != 0u) {
        reply.type = publish.qos == 1u ? FLOWIE_MQTT_PACKET_PUBACK : FLOWIE_MQTT_PACKET_PUBREC;
        reply.packet_id = publish.packet_id;
      }
      if (f->disconnect_first_publish) {
        f->disconnect_first_publish = 0;
        reply.type = FLOWIE_MQTT_PACKET_DISCONNECT;
        reply.packet_id = 0u;
        reply.reason_code = 0x89u; /* A normal server-busy reconnect episode. */
      }
    } else if (packet.type == FLOWIE_MQTT_PACKET_PUBREL) {
      flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
      if (flowie_mqtt_control_packet_parse(&packet, &control) != FLOWIE_MQTT_PARSE_OK)
        return SALTS_EPROTO;
      atomic_fetch_add_explicit(&f->pubrels, 1, memory_order_release);
      reply.type = FLOWIE_MQTT_PACKET_PUBCOMP;
      reply.packet_id = control.packet_id;
    } else return SALTS_EPROTO;
    if (reply.type != 0) {
      /* Two valid QoS2 response stages consume the same command budget. */
      if (f->ack_delay_ms && packet.type != FLOWIE_MQTT_PACKET_CONNECT)
        cmeta_sleep_ms(f->ack_delay_ms);
      status = flowie_mqtt_control_packet_encode(&reply, output, sizeof(output), &written);
      if (status != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
      status = flowie_server_send(&f->server, connection, output, written);
      if (status != SALTS_OK && status != SALTS_ENOENT && status != SALTS_ESHUTDOWN)
        return status;
    }
    memmove(f->input, f->input + consumed, f->input_size - consumed);
    f->input_size -= consumed;
  }
  return SALTS_OK;
}

static void flowie_client_contract_closed(void *user, flowie_connection connection, int status) {
  flowie_client_contract_fixture *f = user;
  (void)connection;
  (void)status;
  atomic_fetch_add_explicit(&f->closed_connections, 1, memory_order_release);
}

static void flowie_client_contract_wait(atomic_int *value, int count) {
  const uint64_t deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) < count &&
         cmeta_monotonic_ms() < deadline) cmeta_sleep_ms(1u);
  check_equal(atomic_load_explicit(value, memory_order_acquire), count);
}

static flowie_mqtt_connect_packet_t flowie_client_contract_connect_packet(void) {
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){(const unsigned char *)"contract", 8u};
  connect.has_username = 1u;
  connect.has_password = 1u;
  connect.username = (flowie_mqtt_span_t){(const unsigned char *)"test", 4u};
  connect.password = (flowie_mqtt_span_t){(const unsigned char *)"initial", 7u};
  return connect;
}

static void flowie_client_contract_start(flowie_client_contract_fixture *f,
    flowie_mqtt_client_transport_t transport, uint64_t timeout_ms) {
  flowie_server_config server = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_client_resilience_config_t resilience = FLOWIE_MQTT_CLIENT_RESILIENCE_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = flowie_client_contract_connect_packet();
  uint16_t port = 0u;
  server.transport = transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WS
                         ? TF_NET_TRANSPORT_WS : TF_NET_TRANSPORT_TCP;
  server.host = "127.0.0.1";
  server.port = 0u;
  server.backlog = 4u;
  server.command_capacity = 8u;
  server.command_bytes_capacity = 8192u;
  server.path = "/mqtt";
  server.websocket_subprotocol = transport == FLOWIE_MQTT_CLIENT_TRANSPORT_WS ? "mqtt" : NULL;
  server.stream = flowie_client_transport_network();
  server.max_message_bytes = FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES;
  server.poll_slice_ms = 1u;
  server.observer = (flowie_observer){flowie_client_contract_open,
      flowie_client_contract_receive, flowie_client_contract_closed, NULL, f};
  check_equal(flowie_server_init(&f->server, &server), SALTS_OK);
  f->server_initialized = 1;
  check_equal(flowie_server_start(&f->server), SALTS_OK);
  f->server_started = 1;
  check_equal(flowie_server_port(&f->server, &port), SALTS_OK);
  config.transport = transport;
  config.host = "127.0.0.1";
  config.port = port;
  config.timeout_ms = timeout_ms;
  config.on_connect = flowie_client_contract_connected;
  config.on_publish = flowie_client_contract_published;
  config.on_error = flowie_client_contract_error;
  config.user_data = f;
  resilience.initial_delay_ms = 10u;
  resilience.max_delay_ms = 40u;
  resilience.max_attempts = 2u;
  resilience.refresh_connect = flowie_client_contract_refresh;
  resilience.on_reconnect = flowie_client_contract_reconnected;
  check_equal(flowie_mqtt_client_create_ex(&config,
                                           f->refresh || f->resilience_enabled ? &resilience : NULL,
                                           &f->client), SALTS_OK);
  check_equal(flowie_mqtt_client_connect(f->client, &connect), SALTS_OK);
  flowie_client_contract_wait(&f->completions[FLOWIE_CONTRACT_CONNECT], 1);
  check_equal(atomic_load(&f->statuses[FLOWIE_CONTRACT_CONNECT]), SALTS_OK);
}

static void flowie_client_contract_publish(flowie_client_contract_fixture *f, uint8_t qos) {
  flowie_mqtt_client_publish_topic_t topic = {0};
  flowie_mqtt_client_publish_topic_vec_t topics = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
  topic.qos = qos;
  topic.topic = (flowie_mqtt_span_t){(const unsigned char *)"contract/topic", 14u};
  topic.payload = (flowie_mqtt_span_t){(const unsigned char *)"value", 5u};
  topics.data = &topic;
  topics.count = 1u;
  check_equal(flowie_mqtt_client_publish(f->client, &topics), SALTS_OK);
}

spec("Flowie client receive and command contracts") {
  static flowie_client_contract_fixture f;
  before_each() {
    memset(&f, 0, sizeof(f));
    for (int i = 0; i < FLOWIE_CONTRACT_CALLBACK_COUNT; ++i) {
      atomic_init(&f.completions[i], 0);
      atomic_init(&f.statuses[i], SALTS_EBUSY);
    }
    atomic_init(&f.connects, 0);
    atomic_init(&f.publishes, 0);
    atomic_init(&f.pubrels, 0);
    atomic_init(&f.bad_passwords, 0);
    atomic_init(&f.closed_connections, 0);
    atomic_init(&f.errors, 0);
    atomic_init(&f.refreshes, 0);
    atomic_init(&f.reason_callback_entered, 0);
    atomic_init(&f.reason_callback_release, 0);
    atomic_init(&f.callback_reason_status, SALTS_EBUSY);
    atomic_init(&f.callback_reason, 0);
  }
  after_each() {
    atomic_store_explicit(&f.reason_callback_release, 1, memory_order_release);
    if (f.client) check_equal(flowie_mqtt_client_try_destroy(
        f.client, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
    if (f.server_started) check_equal(flowie_server_stop(
        &f.server, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
    if (f.server_initialized) check_equal(flowie_server_destroy(&f.server), SALTS_OK);
    check_equal(atomic_load(&f.closed_connections), atomic_load(&f.connects));
  }
  it("allows only CONNACK and AUTH through the CONNECT packet gate") {
    for (int type = 0; type <= 15; ++type)
      check_equal(flowie_mqtt_client_test_connect_packet_admit((flowie_mqtt_packet_type_t)type),
          type == FLOWIE_MQTT_PACKET_CONNACK || type == FLOWIE_MQTT_PACKET_AUTH
              ? SALTS_OK : SALTS_EPROTO);
  }
  it("keeps one idle receive credit across sequential QoS0 commands") {
    flowie_client_contract_start(&f, FLOWIE_MQTT_CLIENT_TRANSPORT_TCP,
                                  FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS);
    const uint64_t deadline = cmeta_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
    while (flowie_mqtt_client_test_receive_admissions(f.client) < 2u &&
           cmeta_monotonic_ms() < deadline) cmeta_sleep_ms(1u);
    check_equal(flowie_mqtt_client_test_receive_admissions(f.client), 2u);
    for (int round = 1; round <= 6; ++round) {
      flowie_client_contract_publish(&f, 0u);
      flowie_client_contract_wait(&f.completions[FLOWIE_CONTRACT_PUBLISH], round);
      flowie_client_contract_wait(&f.publishes, round);
      cmeta_sleep_ms(15u); /* Let the worker resume its idle receive. */
      check_equal(atomic_load(&f.statuses[FLOWIE_CONTRACT_PUBLISH]), SALTS_OK);
      check_equal(flowie_mqtt_client_test_receive_admissions(f.client), 2u);
    }
    /* The original pending read must still settle normally with this PUBACK. */
    flowie_client_contract_publish(&f, 1u);
    flowie_client_contract_wait(&f.completions[FLOWIE_CONTRACT_PUBLISH], 7);
    check_equal(atomic_load(&f.statuses[FLOWIE_CONTRACT_PUBLISH]), SALTS_OK);
    check_equal(atomic_load(&f.errors), 0);
  }
  for (int websocket = 0; websocket <= 1; ++websocket) {
    it("shares one deadline across both QoS2 response stages, websocket=%d", websocket) {
      f.ack_delay_ms = FLOWIE_CONTRACT_STAGE_MS;
      flowie_client_contract_start(&f, websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WS
                                               : FLOWIE_MQTT_CLIENT_TRANSPORT_TCP,
                                    FLOWIE_CONTRACT_COMMAND_MS);
      flowie_client_contract_publish(&f, 2u);
      flowie_client_contract_wait(&f.completions[FLOWIE_CONTRACT_PUBLISH], 1);
      check_equal(atomic_load(&f.statuses[FLOWIE_CONTRACT_PUBLISH]), SALTS_ETIMEDOUT);
      check_equal(atomic_load(&f.publishes), 1);
      check_equal(atomic_load(&f.pubrels), 1);
      check_equal(atomic_load(&f.errors), 0);
    }
  }
  it("owns refreshed credentials before the next reconnect callback reuses their storage") {
    f.refresh = 1;
    flowie_client_contract_start(&f, FLOWIE_MQTT_CLIENT_TRANSPORT_TCP,
                                  FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS);
    flowie_client_contract_wait(&f.completions[FLOWIE_CONTRACT_RECONNECT], 2);
    check_equal(atomic_load(&f.statuses[FLOWIE_CONTRACT_RECONNECT]), SALTS_OK);
    check_equal(atomic_load(&f.refreshes), 1);
    check_equal(atomic_load(&f.connects), 3);
    check_equal(atomic_load(&f.bad_passwords), 0);
    check_equal(flowie_mqtt_client_is_connected(f.client), 1);
  }
  for (int websocket = 0; websocket <= 1; ++websocket) {
    it("preserves reconnect credentials after a duplicate CONNECT, websocket=%d", websocket) {
      f.resilience_enabled = 1;
      f.disconnect_first_publish = 1;
      flowie_client_contract_start(&f, websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WS
                                               : FLOWIE_MQTT_CLIENT_TRANSPORT_TCP,
                                    FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS);
      flowie_mqtt_connect_packet_t duplicate = flowie_client_contract_connect_packet();
      duplicate.password = (flowie_mqtt_span_t){(const unsigned char *)"rejected", 8u};
      check_equal(flowie_mqtt_client_connect(f.client, &duplicate), SALTS_OK);
      flowie_client_contract_wait(&f.completions[FLOWIE_CONTRACT_CONNECT], 2);
      check_equal(atomic_load(&f.statuses[FLOWIE_CONTRACT_CONNECT]), SALTS_EALREADY);
      check_equal(flowie_mqtt_client_is_connected(f.client), 1);
      check_equal(atomic_load(&f.connects), 1);
      flowie_client_contract_publish(&f, 1u);
      flowie_client_contract_wait(&f.completions[FLOWIE_CONTRACT_PUBLISH], 1);
      check_equal(atomic_load(&f.statuses[FLOWIE_CONTRACT_PUBLISH]), SALTS_ECONNRESET);
      flowie_client_contract_wait(&f.completions[FLOWIE_CONTRACT_RECONNECT], 1);
      check_equal(atomic_load(&f.statuses[FLOWIE_CONTRACT_RECONNECT]), SALTS_OK);
      check_equal(atomic_load(&f.bad_passwords), 0);
      check_equal(atomic_load(&f.connects), 2);
      check_equal(atomic_load(&f.publishes), 1);
      check_equal(flowie_mqtt_client_is_connected(f.client), 1);
    }
  }
  it("keeps the active DISCONNECT reason private to its worker callback") {
    f.probe_reason = 1;
    f.disconnect_first_publish = 1;
    flowie_client_contract_start(&f, FLOWIE_MQTT_CLIENT_TRANSPORT_TCP,
                                  FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS);
    flowie_client_contract_publish(&f, 1u);
    flowie_client_contract_wait(&f.reason_callback_entered, 1);
    uint8_t reason = 0u;
    const int status = flowie_mqtt_client_server_disconnect_reason(f.client, &reason);
    atomic_store_explicit(&f.reason_callback_release, 1, memory_order_release);
    flowie_client_contract_wait(&f.completions[FLOWIE_CONTRACT_PUBLISH], 1);
    check_equal(status, SALTS_EBUSY);
    check_equal(reason, 0u);
    check_equal(atomic_load(&f.callback_reason_status), SALTS_OK);
    check_equal(atomic_load(&f.callback_reason), 0x89u);
    check_equal(atomic_load(&f.statuses[FLOWIE_CONTRACT_PUBLISH]), SALTS_ECONNRESET);
    check_equal(flowie_mqtt_client_server_disconnect_reason(f.client, &reason), SALTS_EBUSY);
  }
}
#endif

spec("Flowie MQTT client CNet and CHTTP transports") {
#if !defined(FLOWIE_CLIENT_FAULT_TEST)
  it("stops during an unacknowledged QoS1 PUBLISH without replay or early release") {
    flowie_client_transport_qos_shutdown_case(1u);
  }
  it("stops during an unacknowledged QoS2 PUBLISH without replay or early release") {
    flowie_client_transport_qos_shutdown_case(2u);
  }
#endif
#if defined(FLOWIE_CLIENT_FAULT_TEST)
  it("retains the Client across injected ENOBUFS and progresses close on the original Owner") {
    flowie_client_transport_injected_full_close();
  }
  it("destroys a genuinely stopped native backend despite an observed callback error") {
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_t *client = NULL;
    const unsigned before = flowie_mqtt_client_test_native_stop_reported_errors();
    config.host = "127.0.0.1";
    config.port = 1883;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
    check_equal(flowie_mqtt_client_test_set_native_stop_mode(client, 1), SALTS_OK);
    check_equal(flowie_mqtt_client_try_destroy(
        client, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(flowie_mqtt_client_test_native_stop_reported_errors(), before + 1u);
  }
  it("retains a real WebSocket backend when CHttp destroy times out") {
    flowie_client_transport_ws_destroy_timeout_case(0);
  }
  it("retains a real WSS TLS backend until CHttp native teardown succeeds") {
    flowie_client_transport_ws_destroy_timeout_case(1);
  }
  it("retains the native backend on stop timeout and releases only after real retry") {
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_t *client = NULL;
    int status;
    config.host = "127.0.0.1";
    config.port = 1883;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
    check_equal(flowie_mqtt_client_test_set_native_stop_mode(client, 2), SALTS_OK);
    status = flowie_mqtt_client_try_destroy(client, 150u);
    check(status == SALTS_ETIMEDOUT || status == SALTS_EBUSY);
    check(flowie_mqtt_client_test_native_stop_timeout_hits(client) > 0u);
    /* Still-live native owner is retryable: no force-free on timeout. */
    check_equal(flowie_mqtt_client_test_set_native_stop_mode(client, 0), SALTS_OK);
    check_equal(flowie_mqtt_client_try_destroy(
        client, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  }
#endif
  it("selects an authorized physical MQTT broker through CNet Destination Policy") {
    flowie_client_transport_case_ex(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                    FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID,
                                    FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS, SALTS_OK, 1, 1);
  }

  it("rejects invalid and unsupported remote policy profiles before CONNECT") {
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_t *client = NULL;
    flowie_mqtt_client_remote_endpoint_t endpoints[] = {
        {10u, "127.0.0.1", 1883u, 1u, 0u, 1u},
        {10u, "127.0.0.1", 1884u, 1u, 0u, 1u}};
    flowie_mqtt_client_destination_policy_t policy = {
        sizeof(policy), FLOWIE_MQTT_CLIENT_DESTINATION_VERSION,
        FLOWIE_MQTT_DESTINATION_EXPLICIT, endpoints, 2u,
        1u, UINT64_MAX, 0u, 10u, 0u, 0u};
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TCP;
    config.host = "127.0.0.1";
    config.port = 1883;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
    check_equal(flowie_mqtt_client_set_destination_policy(client, &policy), SALTS_EINVAL);
    endpoints[1].endpoint_id = 20u;
    policy.kind = FLOWIE_MQTT_DESTINATION_STRICT_KEY;
    check_equal(flowie_mqtt_client_set_destination_policy(client, &policy), SALTS_EINVAL);
    policy.key_known = 1u;
    check_equal(flowie_mqtt_client_set_destination_policy(client, &policy), SALTS_OK);
    check_equal(flowie_mqtt_client_set_destination_policy(client, &policy), SALTS_EBUSY);
    flowie_mqtt_client_destroy(client);

    client = NULL;
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_WS;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
    check_equal(flowie_mqtt_client_set_destination_policy(client, &policy), SALTS_ENOTSUP);
    flowie_mqtt_client_destroy(client);
  }


  it("pins the CNet Managed Dial destination across three MQTT session episodes") {
    flowie_client_transport_case_ex(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                    FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID,
                                    FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS, SALTS_OK, 1, 3);
  }

  it("recycles one bounded Owner-local CNet Manager record across three MQTT sessions") {
    flowie_client_transport_case_ex(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                    FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID,
                                    FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS, SALTS_OK, 0, 3);
  }

  it("fences MQTT TCP commands after callback-originated stop and drains externally") {
    flowie_client_transport_self_stop_case(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP,
                                           TF_NET_TRANSPORT_TCP);
  }

  it("fences MQTT WebSocket commands after callback-originated stop and drains externally") {
    flowie_client_transport_self_stop_case(FLOWIE_MQTT_CLIENT_TRANSPORT_WS,
                                           TF_NET_TRANSPORT_WS);
  }

  it("safely drains a pending MQTT CONNECT/CONNACK before releasing Client") {
    flowie_client_transport_destroy_pending_connack();
  }

  it("retains a Client after nonblocking destroy and retries on the same Owner") {
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_t *client = NULL;
    config.host = "127.0.0.1";
    config.port = 1883;
    check_equal(flowie_mqtt_client_create(&config, &client), SALTS_OK);
    check_equal(flowie_mqtt_client_try_destroy(NULL, 0u), SALTS_EINVAL);
    check_equal(flowie_mqtt_client_try_destroy(client, 0u), SALTS_EBUSY);
    check_equal(flowie_mqtt_client_try_destroy(client,
                FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  }

  it("runs CONNECT, PING, and DISCONNECT over CNet TCP") {
    flowie_client_transport_case(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                 FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID,
                                 FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS, SALTS_OK);
  }

  it("recycles the same CHttp Owner over three sequential MQTT WS sessions") {
    flowie_client_transport_case_ex(FLOWIE_MQTT_CLIENT_TRANSPORT_WS, TF_NET_TRANSPORT_WS,
                                    FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID,
                                    FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS, SALTS_OK, 0, 3);
  }

  it("runs MQTT over a CHTTP WebSocket with the mqtt subprotocol") {
    flowie_client_transport_case(FLOWIE_MQTT_CLIENT_TRANSPORT_WS, TF_NET_TRANSPORT_WS,
                                 FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID,
                                 FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS, SALTS_OK);
  }

  it("recovers Manager and Dial after three unsuccessful MQTT CONNACK deadlines") {
    flowie_client_transport_case_ex(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                    FLOWIE_CLIENT_TRANSPORT_CONNACK_SILENT,
                                    FLOWIE_CLIENT_TRANSPORT_SHORT_TIMEOUT_MS,
                                    SALTS_ETIMEDOUT, 0, 3);
  }

  it("recovers Manager and Dial after three invalid pre-CONNACK responses") {
    flowie_client_transport_case_ex(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                    FLOWIE_CLIENT_TRANSPORT_CONNACK_UNEXPECTED_PACKET,
                                    FLOWIE_CLIENT_TRANSPORT_SHORT_TIMEOUT_MS,
                                    SALTS_EPROTO, 0, 3);
  }

  it("reports a missing CONNACK as a timeout rather than a protocol error") {
    flowie_client_transport_case(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                 FLOWIE_CLIENT_TRANSPORT_CONNACK_SILENT,
                                 FLOWIE_CLIENT_TRANSPORT_SHORT_TIMEOUT_MS, SALTS_ETIMEDOUT);
  }

  it("reports an unexpected packet before CONNACK as a protocol error") {
    flowie_client_transport_case(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                 FLOWIE_CLIENT_TRANSPORT_CONNACK_UNEXPECTED_PACKET,
                                 FLOWIE_CLIENT_TRANSPORT_SHORT_TIMEOUT_MS, SALTS_EPROTO);
  }


#if defined(FLOWIE_P384_SHA384_ACCEPTANCE_TESTS)
  for (int secure_websocket = 0; secure_websocket <= 1; ++secure_websocket) {
    it("authenticates P-384 SHA-384 issuing CA and P-256 leaf over MQTT TLS/WSS, websocket=%d",
       secure_websocket) {
      flowie_client_transport_case_ex_fixture(
          secure_websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WSS : FLOWIE_MQTT_CLIENT_TRANSPORT_TLS,
          secure_websocket ? TF_NET_TRANSPORT_WSS : TF_NET_TRANSPORT_TLS,
          FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
          SALTS_OK, 0, 1, FLOWIE_P384_SHA384_CA, FLOWIE_P384_SHA384_LEAF,
          FLOWIE_P384_SHA384_KEY, NULL, NULL, 0);
    }
    it("authenticates a P-384 root/intermediate and P-256 leaf over MQTT TLS/WSS, websocket=%d",
       secure_websocket) {
      flowie_client_transport_case_ex_fixture(
          secure_websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WSS : FLOWIE_MQTT_CLIENT_TRANSPORT_TLS,
          secure_websocket ? TF_NET_TRANSPORT_WSS : TF_NET_TRANSPORT_TLS,
          FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
          SALTS_OK, 0, 1, FLOWIE_P384_SHA384_CA,
          FLOWIE_P384_SHA384_INTERMEDIATE_CHAIN, FLOWIE_P384_SHA384_KEY,
          NULL, NULL, 0);
    }
    const char *const invalid_chains[] = {
        FLOWIE_P384_SHA384_INTERMEDIATE_LEAF,
        FLOWIE_P384_SHA384_INVALID_BASIC_CA_CHAIN,
        FLOWIE_P384_SHA384_INVALID_KEY_USAGE_CHAIN,
        FLOWIE_P384_SHA384_INVALID_EKU_LEAF,
        FLOWIE_P384_SHA384_UNSUPPORTED_SIGNATURE_OID,
        FLOWIE_P384_SHA384_MALFORMED_SAN_LEAF};
    const char *const chain_failures[] = {
        "an omitted intermediate", "invalid intermediate basic constraints",
        "invalid intermediate key usage", "invalid leaf extended key usage",
        "an unsupported certificate signature OID", "malformed subjectAltName DER"};
    for (size_t chain_case = 0u;
         chain_case < sizeof(invalid_chains) / sizeof(invalid_chains[0]); ++chain_case) {
      const char *chain = invalid_chains[chain_case];
      const char *failure = chain_failures[chain_case];
      it("rejects %s before MQTT CONNECT over TLS/WSS, websocket=%d",
         failure, secure_websocket) {
        flowie_client_transport_case_ex_fixture(
            secure_websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WSS
                             : FLOWIE_MQTT_CLIENT_TRANSPORT_TLS,
            secure_websocket ? TF_NET_TRANSPORT_WSS : TF_NET_TRANSPORT_TLS,
            FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
            SALTS_OK, 0, 1, FLOWIE_P384_SHA384_CA, chain, FLOWIE_P384_SHA384_KEY,
            NULL, NULL, 1);
      }
    }
    it("rejects wrong P-384 SHA-384 issuing CA before MQTT CONNECT over TLS/WSS, websocket=%d",
       secure_websocket) {
      flowie_client_transport_case_ex_fixture(
          secure_websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WSS : FLOWIE_MQTT_CLIENT_TRANSPORT_TLS,
          secure_websocket ? TF_NET_TRANSPORT_WSS : TF_NET_TRANSPORT_TLS,
          FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
          SALTS_OK, 0, 1, FLOWIE_P384_SHA384_CA, FLOWIE_P384_SHA384_LEAF,
          FLOWIE_P384_SHA384_KEY, FLOWIE_P384_SHA384_WRONG_CA, NULL, 1);
    }
    it("rejects a mismatched P-384 SHA-384 TLS/WSS identity before MQTT CONNECT, websocket=%d",
       secure_websocket) {
      /* TCP/TLS separates the logical authority from the loopback destination
       * through CNet policy. WSS has no destination-policy API, so its server
       * certificate deliberately omits the 127.0.0.1 IP SAN. Both paths reach
       * a real TLS handshake and reject identity before MQTT CONNECT. */
      flowie_client_transport_case_ex_fixture(
          secure_websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WSS : FLOWIE_MQTT_CLIENT_TRANSPORT_TLS,
          secure_websocket ? TF_NET_TRANSPORT_WSS : TF_NET_TRANSPORT_TLS,
          FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
          SALTS_OK, secure_websocket ? 0 : 1, 1, FLOWIE_P384_SHA384_CA,
          secure_websocket ? FLOWIE_P384_SHA384_DNS_ONLY_LEAF : FLOWIE_P384_SHA384_LEAF,
          FLOWIE_P384_SHA384_KEY, NULL, secure_websocket ? NULL : "wrong.example", 1);
    }
    it("rejects a tampered P-384 SHA-384 TLS/WSS leaf before MQTT CONNECT, websocket=%d",
       secure_websocket) {
      flowie_client_transport_case_ex_fixture(
          secure_websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WSS : FLOWIE_MQTT_CLIENT_TRANSPORT_TLS,
          secure_websocket ? TF_NET_TRANSPORT_WSS : TF_NET_TRANSPORT_TLS,
          FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
          SALTS_OK, 0, 1, FLOWIE_P384_SHA384_CA, FLOWIE_P384_SHA384_TAMPERED_LEAF,
          FLOWIE_P384_SHA384_KEY, NULL, NULL, 1);
    }
    for (size_t validity_case = 0u; validity_case < 2u; ++validity_case) {
      const char *leaf = validity_case == 0u ? FLOWIE_P384_SHA384_EXPIRED_LEAF
                                             : FLOWIE_P384_SHA384_FUTURE_LEAF;
      const char *validity = validity_case == 0u ? "expired" : "not-yet-valid";
      it("rejects a %s P-384 SHA-384 TLS/WSS leaf before MQTT CONNECT, websocket=%d",
         validity, secure_websocket) {
        flowie_client_transport_case_ex_fixture(
            secure_websocket ? FLOWIE_MQTT_CLIENT_TRANSPORT_WSS
                             : FLOWIE_MQTT_CLIENT_TRANSPORT_TLS,
            secure_websocket ? TF_NET_TRANSPORT_WSS : TF_NET_TRANSPORT_TLS,
            FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
            SALTS_OK, 0, 1, FLOWIE_P384_SHA384_CA, leaf, FLOWIE_P384_SHA384_KEY,
            NULL, NULL, 1);
      }
    }
  }
#endif

  it("classifies a TLS authority mismatch as SECURITY without automatic MQTT retry") {
    flowie_client_transport_tls_identity_fail_closed();
  }

  it("reports an abrupt TLS EOF after CONNECT as an aborted connection") {
    flowie_client_transport_abrupt_tls_close();
  }
}
