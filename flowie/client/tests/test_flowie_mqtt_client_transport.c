#include "flowie_mqtt_client.h"
#include "flow_net_runtime.h"
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
  FLOWIE_CLIENT_TRANSPORT_CONNACK_UNEXPECTED_PACKET
} flowie_client_transport_connack_mode;

typedef struct flowie_client_transport_broker {
  tf_net_server *server;
  unsigned char input[FLOWIE_CLIENT_TRANSPORT_TEST_BUFFER_BYTES];
  size_t input_size;
  atomic_int opens;
  atomic_int connects;
  atomic_int pings;
  atomic_int disconnects;
  atomic_int closes;
  atomic_int error;
  flowie_client_transport_connack_mode connack_mode;
} flowie_client_transport_broker;

typedef struct flowie_client_transport_probe {
  atomic_int done;
  atomic_int connect_status;
  atomic_int ping_status;
  atomic_int disconnect_status;
  atomic_int submit_status;
  atomic_int errors;
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

static int flowie_client_transport_open(void *user, tf_net_connection connection,
                                        const tf_net_peer_info *peer) {
  flowie_client_transport_broker *broker = (flowie_client_transport_broker *)user;
  (void)connection;
  if (broker == NULL || peer == NULL) return SALTS_EINVAL;
  atomic_fetch_add_explicit(&broker->opens, 1, memory_order_relaxed);
  return SALTS_OK;
}

static int flowie_client_transport_receive(void *user, tf_net_connection connection,
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
      atomic_fetch_add_explicit(&broker->connects, 1, memory_order_relaxed);
      if (broker->connack_mode == FLOWIE_CLIENT_TRANSPORT_CONNACK_SILENT)
        status = SALTS_OK;
      else if (broker->connack_mode == FLOWIE_CLIENT_TRANSPORT_CONNACK_UNEXPECTED_PACKET)
        status = tf_net_server_send(broker->server, connection, pingresp, sizeof(pingresp));
      else status = tf_net_server_send(broker->server, connection, connack, sizeof(connack));
      break;
    case FLOWIE_MQTT_PACKET_PINGREQ:
      atomic_fetch_add_explicit(&broker->pings, 1, memory_order_relaxed);
      status = tf_net_server_send(broker->server, connection, pingresp, sizeof(pingresp));
      break;
    case FLOWIE_MQTT_PACKET_DISCONNECT:
      atomic_fetch_add_explicit(&broker->disconnects, 1, memory_order_relaxed);
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

static void flowie_client_transport_close(void *user, tf_net_connection connection, int status) {
  flowie_client_transport_broker *broker = (flowie_client_transport_broker *)user;
  (void)connection;
  if (broker == NULL) return;
  if (status != SALTS_OK && status != SALTS_EOF && status != SALTS_ECONNRESET)
    atomic_store_explicit(&broker->error, status, memory_order_release);
  atomic_fetch_add_explicit(&broker->closes, 1, memory_order_relaxed);
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

static cnet_client_config flowie_client_transport_network(void) {
  const cnet_client_config config = {.backend = flowie_client_transport_backend(),
                                     .connection_capacity = 4u,
                                     .command_capacity = 16u,
                                     .request_capacity = 8u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 16u,
                                     .max_send_bytes = 8192u,
                                     .receive_buffer_bytes = 8192u,
                                     .connect_timeout_ms =
                                         FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
                                     .read_timeout_ms =
                                         FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS,
                                     .write_timeout_ms =
                                         FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS};
  return config;
}

static void flowie_client_transport_case(flowie_mqtt_client_transport_t client_transport,
                                         tf_net_transport server_transport,
                                         flowie_client_transport_connack_mode connack_mode,
                                         uint64_t client_timeout_ms, int expected_connect_status) {
  static const unsigned char client_id[] = "cnet-client-test";
  flowie_client_transport_broker broker = {0};
  flowie_client_transport_probe probe = {0};
  tf_net_server server = {0};
  tf_net_server_config server_config = TF_NET_SERVER_CONFIG_INIT;
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mqtt_client_t *client = NULL;
  uint16_t port = 0u;
  uint64_t deadline;

  atomic_init(&broker.opens, 0);
  atomic_init(&broker.connects, 0);
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
  broker.server = &server;
  broker.connack_mode = connack_mode;

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
  server_config.observer = (tf_net_observer){flowie_client_transport_open,
                                             flowie_client_transport_receive,
                                             flowie_client_transport_close, NULL, &broker};
  check_equal(tf_net_server_init(&server, &server_config), SALTS_OK);
  check_equal(tf_net_server_start(&server), SALTS_OK);
  check_equal(tf_net_server_port(&server, &port), SALTS_OK);

  client_config.transport = client_transport;
  client_config.host = "127.0.0.1";
  client_config.port = (int)port;
  client_config.path = "/mqtt";
  client_config.timeout_ms = client_timeout_ms;
  client_config.socket_recv_buffer_bytes = 32768u;
  client_config.socket_send_buffer_bytes = 32768u;
  client_config.on_connect = flowie_client_transport_connect_complete;
  client_config.on_ping = flowie_client_transport_ping_complete;
  client_config.on_disconnect = flowie_client_transport_disconnect_complete;
  client_config.on_error = flowie_client_transport_error;
  client_config.user_data = &probe;
  check_equal(flowie_mqtt_client_create(&client_config, &client), SALTS_OK);

  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);
  deadline = salts_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(&probe.done, memory_order_acquire) &&
         salts_monotonic_ms() < deadline)
    salts_sleep_ms(1u);

  check_equal(atomic_load_explicit(&probe.done, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed),
              expected_connect_status);
  if (expected_connect_status == SALTS_OK) {
    check_equal(atomic_load_explicit(&probe.ping_status, memory_order_relaxed), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.disconnect_status, memory_order_relaxed), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.submit_status, memory_order_relaxed), SALTS_OK);
  }
  check_equal(atomic_load_explicit(&probe.errors, memory_order_relaxed), 0);
  check_equal(atomic_load_explicit(&broker.opens, memory_order_relaxed), 1);
  check_equal(atomic_load_explicit(&broker.connects, memory_order_relaxed), 1);
  check_equal(atomic_load_explicit(&broker.pings, memory_order_relaxed),
              expected_connect_status == SALTS_OK ? 1 : 0);
  check_equal(atomic_load_explicit(&broker.disconnects, memory_order_relaxed),
              expected_connect_status == SALTS_OK ? 1 : 0);
  check_equal(atomic_load_explicit(&broker.error, memory_order_relaxed), SALTS_OK);

  flowie_mqtt_client_destroy(client);
  check_equal(tf_net_server_stop(&server, FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(tf_net_server_destroy(&server), SALTS_OK);
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

  atomic_init(&probe.done, 0);
  atomic_init(&probe.connect_status, SALTS_EBUSY);
  atomic_init(&probe.ping_status, SALTS_EBUSY);
  atomic_init(&probe.disconnect_status, SALTS_EBUSY);
  atomic_init(&probe.submit_status, SALTS_EBUSY);
  atomic_init(&probe.errors, 0);

  check_equal(tls_test_write_ca_file(ca_path, sizeof(ca_path)), 0);
  check_equal(flow_tls_test_server_start_abrupt(&server), 0);

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
  check_equal(flowie_mqtt_client_create(&client_config, &client), SALTS_OK);

  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  check_equal(flowie_mqtt_client_connect(client, &connect), SALTS_OK);
  deadline = salts_monotonic_ms() + FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(&probe.done, memory_order_acquire) &&
         salts_monotonic_ms() < deadline)
    salts_sleep_ms(1u);

  check_equal(atomic_load_explicit(&probe.done, memory_order_acquire), 1);
  check_equal(atomic_load_explicit(&probe.connect_status, memory_order_relaxed),
              SALTS_ECONNABORTED);
  check_equal(atomic_load_explicit(&probe.errors, memory_order_relaxed), 0);
  flow_mtls_test_server_join(&server);
  check_equal(server.status, 0);
  check_greater(server.request_size, (size_t)0u);
  check_equal(server.request[0] >> 4u, FLOWIE_MQTT_PACKET_CONNECT);

  flowie_mqtt_client_destroy(client);
  tls_test_remove_file(ca_path);
}

spec("Flowie MQTT client CNet and CHTTP transports") {
  it("runs CONNECT, PING, and DISCONNECT over CNet TCP") {
    flowie_client_transport_case(FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, TF_NET_TRANSPORT_TCP,
                                 FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID,
                                 FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS, SALTS_OK);
  }

  it("runs MQTT over a CHTTP WebSocket with the mqtt subprotocol") {
    flowie_client_transport_case(FLOWIE_MQTT_CLIENT_TRANSPORT_WS, TF_NET_TRANSPORT_WS,
                                 FLOWIE_CLIENT_TRANSPORT_CONNACK_VALID,
                                 FLOWIE_CLIENT_TRANSPORT_TEST_TIMEOUT_MS, SALTS_OK);
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

  it("reports an abrupt TLS EOF after CONNECT as an aborted connection") {
    flowie_client_transport_abrupt_tls_close();
  }
}
