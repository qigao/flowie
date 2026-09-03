#include "flow_net_runtime.h"

#include "../../tests/flowie_test_socket.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/error_codes.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FLOW_NET_RUNTIME_TEST_TIMEOUT_MS 3000u
#define FLOW_NET_RUNTIME_TEST_CLOSE_STATUS SALTS_ECANCELED

typedef struct flow_net_runtime_probe {
  atomic_uint slot;
  atomic_uint generation;
  atomic_int opened;
  atomic_int received;
  atomic_int closed;
  atomic_int close_count;
  atomic_int close_status;
  unsigned char payload[64];
  size_t payload_size;
} flow_net_runtime_probe;

typedef struct flow_net_packet_probe {
  atomic_int received;
  unsigned char payload[64];
  size_t payload_size;
} flow_net_packet_probe;

static native_io_backend_kind flow_net_runtime_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config flow_net_runtime_stream_config(void) {
  const cnet_client_config config = {.backend = flow_net_runtime_backend(),
                                     .connection_capacity = 4u,
                                     .command_capacity = 16u,
                                     .request_capacity = 8u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 16u,
                                     .max_send_bytes = 8192u,
                                     .receive_buffer_bytes = 8192u};
  return config;
}

static chttp_websocket_client_config flow_net_runtime_websocket_client_config(void) {
  const chttp_websocket_client_config config = {.size = sizeof(config),
                                                .network =
                                                    flow_net_runtime_stream_config(),
                                                .max_frame_bytes = 4096u,
                                                .max_message_bytes = 4096u,
                                                .max_buffered_input_bytes = 8192u,
                                                .max_handshake_header_bytes = 4096u,
                                                .event_capacity = 8u};
  return config;
}

static int flow_net_runtime_open(void *user, tf_net_connection connection,
                                 const tf_net_peer_info *peer) {
  flow_net_runtime_probe *probe = (flow_net_runtime_probe *)user;
  if (probe == NULL || peer == NULL) return SALTS_EINVAL;
  atomic_store_explicit(&probe->slot, connection.slot, memory_order_relaxed);
  atomic_store_explicit(&probe->generation, connection.generation, memory_order_relaxed);
  atomic_store_explicit(&probe->opened, 1, memory_order_release);
  return SALTS_OK;
}

static int flow_net_runtime_receive(void *user, tf_net_connection connection, const void *data,
                                    size_t size) {
  flow_net_runtime_probe *probe = (flow_net_runtime_probe *)user;
  (void)connection;
  if (probe == NULL || data == NULL || size > sizeof(probe->payload)) return SALTS_EINVAL;
  memcpy(probe->payload, data, size);
  probe->payload_size = size;
  atomic_store_explicit(&probe->received, 1, memory_order_release);
  return SALTS_OK;
}

static void flow_net_runtime_close(void *user, tf_net_connection connection, int status) {
  flow_net_runtime_probe *probe = (flow_net_runtime_probe *)user;
  (void)connection;
  if (probe == NULL) return;
  atomic_store_explicit(&probe->close_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&probe->close_count, 1, memory_order_relaxed);
  atomic_store_explicit(&probe->closed, 1, memory_order_release);
}

static int flow_net_runtime_wait(atomic_int *value) {
  const uint64_t deadline = salts_monotonic_ms() + FLOW_NET_RUNTIME_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) == 0 &&
         salts_monotonic_ms() < deadline)
    salts_sleep_ms(1u);
  return atomic_load_explicit(value, memory_order_acquire) != 0 ? SALTS_OK : SALTS_ETIMEDOUT;
}

static void flow_net_packet_receive(void *user, cnet_packet_endpoint *endpoint,
                                    cnet_packet_session session, const cnet_receive_view *view) {
  flow_net_packet_probe *probe = (flow_net_packet_probe *)user;
  (void)endpoint;
  (void)session;
  if (probe == NULL || view == NULL || view->data == NULL ||
      view->size > sizeof(probe->payload))
    return;
  memcpy(probe->payload, view->data, view->size);
  probe->payload_size = view->size;
  atomic_store_explicit(&probe->received, 1, memory_order_release);
}

static cnet_packet_endpoint_config flow_net_packet_config(cnet_packet_protocol protocol,
                                                          flow_net_packet_probe *probe) {
  cnet_packet_endpoint_config config = CNET_PACKET_ENDPOINT_CONFIG_INIT;
  config.protocol = protocol;
  config.session_capacity = 4u;
  config.datagram.backend = flow_net_runtime_backend();
  config.datagram.host = "127.0.0.1";
  config.datagram.port = 0u;
  config.datagram.send_capacity = 32u;
  config.datagram.request_capacity = 33u;
  config.datagram.completion_batch_capacity = 16u;
  config.datagram.max_datagram_bytes = 1500u;
  config.datagram.receive_buffer_bytes = 1500u;
  config.kcp.mtu = 512u;
  config.kcp.send_window = 32u;
  config.kcp.receive_window = 32u;
  config.kcp.send_segment_capacity = 64u;
  config.kcp.max_message_bytes = 4096u;
  config.observer.on_receive = flow_net_packet_receive;
  config.observer.user = probe;
  return config;
}

static tf_net_server_config flow_net_runtime_config(flow_net_runtime_probe *probe,
                                                    tf_net_transport transport) {
  tf_net_server_config config = TF_NET_SERVER_CONFIG_INIT;
  config.transport = transport;
  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = 4u;
  config.path = "/mqtt";
  config.stream = flow_net_runtime_stream_config();
  config.packet = flow_net_packet_config(
      transport == TF_NET_TRANSPORT_UDP ? CNET_PACKET_UDP : CNET_PACKET_KCP, NULL);
  config.command_capacity = 8u;
  config.command_bytes_capacity = 8192u;
  config.max_message_bytes = 4096u;
  config.poll_slice_ms = 1u;
  config.observer.on_open = flow_net_runtime_open;
  config.observer.on_receive = flow_net_runtime_receive;
  config.observer.on_close = flow_net_runtime_close;
  config.observer.user = probe;
  return config;
}

static void flow_net_runtime_packet_round_trip(tf_net_transport transport,
                                               cnet_packet_protocol protocol,
                                               uint32_t conversation) {
  static const unsigned char inbound[] = "packet-inbound";
  static const unsigned char outbound[] = "packet-outbound";
  flow_net_runtime_probe server_probe = {0};
  flow_net_packet_probe client_probe = {0};
  tf_net_server server = {0};
  tf_net_server_config server_config = flow_net_runtime_config(&server_probe, transport);
  cnet_packet_endpoint client = {0};
  cnet_packet_endpoint_config client_config = flow_net_packet_config(protocol, &client_probe);
  cnet_packet_session session = {0};
  cnet_datagram_peer peer = {0};
  tf_net_connection connection;
  uint16_t port = 0u;
  size_t events = 0u;
  size_t attempts;

  check_equal(tf_net_server_init(&server, &server_config), SALTS_OK);
  check_equal(tf_net_server_start(&server), SALTS_OK);
  check_equal(tf_net_server_port(&server, &port), SALTS_OK);
  check_true(port != 0u);
  check_equal(cnet_packet_endpoint_init(&client, &client_config), SALTS_OK);
  peer.family = CNET_DATAGRAM_ADDRESS_IPV4;
  peer.port = port;
  peer.address[0] = 127u;
  peer.address[3] = 1u;
  check_equal(cnet_packet_session_open(&client, &peer, conversation, &session), SALTS_OK);
  check_equal(cnet_packet_send(&client, session, inbound, sizeof(inbound)), SALTS_OK);
  for (attempts = 0u; attempts < FLOW_NET_RUNTIME_TEST_TIMEOUT_MS &&
                      atomic_load_explicit(&server_probe.received, memory_order_acquire) == 0;
       ++attempts)
    check_equal(cnet_packet_poll(&client, 1u, &events), SALTS_OK);
  check_equal(flow_net_runtime_wait(&server_probe.opened), SALTS_OK);
  check_equal(flow_net_runtime_wait(&server_probe.received), SALTS_OK);
  check_equal(server_probe.payload_size, sizeof(inbound));
  check_equal(memcmp(server_probe.payload, inbound, sizeof(inbound)), 0);

  connection.slot = atomic_load_explicit(&server_probe.slot, memory_order_relaxed);
  connection.generation = atomic_load_explicit(&server_probe.generation, memory_order_relaxed);
  check_equal(tf_net_server_send(&server, connection, outbound, sizeof(outbound)), SALTS_OK);
  for (attempts = 0u; attempts < FLOW_NET_RUNTIME_TEST_TIMEOUT_MS &&
                      atomic_load_explicit(&client_probe.received, memory_order_acquire) == 0;
       ++attempts)
    check_equal(cnet_packet_poll(&client, 1u, &events), SALTS_OK);
  check_equal(flow_net_runtime_wait(&client_probe.received), SALTS_OK);
  check_equal(client_probe.payload_size, sizeof(outbound));
  check_equal(memcmp(client_probe.payload, outbound, sizeof(outbound)), 0);

  check_equal(tf_net_server_close(&server, connection, FLOW_NET_RUNTIME_TEST_CLOSE_STATUS),
              SALTS_OK);
  check_equal(flow_net_runtime_wait(&server_probe.closed), SALTS_OK);
  check_equal(atomic_load_explicit(&server_probe.close_status, memory_order_relaxed),
              FLOW_NET_RUNTIME_TEST_CLOSE_STATUS);
  check_equal(atomic_load_explicit(&server_probe.close_count, memory_order_relaxed), 1);

  check_equal(tf_net_server_stop(&server, FLOW_NET_RUNTIME_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(tf_net_server_destroy(&server), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&client, FLOW_NET_RUNTIME_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&client), SALTS_OK);
}

spec("Flowie CNet and CHTTP transport runtime") {
  it("formats copied IPv4 peer metadata without transport-owned pointers") {
    tf_net_peer_info peer = {0};
    char text[TF_NET_PEER_TEXT_CAPACITY];
    peer.peer.family = CNET_DATAGRAM_ADDRESS_IPV4;
    peer.peer.port = 1883u;
    peer.peer.address[0] = 127u;
    peer.peer.address[3] = 1u;
    check_equal(tf_net_peer_format(&peer, text, sizeof(text)), SALTS_OK);
    check_equal(strcmp(text, "127.0.0.1:1883"), 0);
  }

  it("accepts TCP and admits a copied send from a non-owner thread") {
    static const unsigned char inbound[] = "cnet-inbound";
    static const unsigned char outbound[] = "cnet-outbound";
    flow_net_runtime_probe probe = {0};
    tf_net_server server = {0};
    tf_net_server_config config = flow_net_runtime_config(&probe, TF_NET_TRANSPORT_TCP);
    tf_net_connection connection;
    flowie_test_socket_t client = FLOWIE_TEST_INVALID_SOCKET;
    unsigned char received[sizeof(outbound)] = {0};
    uint16_t port = 0u;

    config.stream_socket_options.receive_buffer_bytes = 32768u;
    config.stream_socket_options.send_buffer_bytes = 32768u;
    config.stream_socket_options.keepalive = 1;
    config.stream_socket_options.linger = 1;

    check_equal(tf_net_server_init(&server, &config), SALTS_OK);
    check_equal(tf_net_server_start(&server), SALTS_OK);
    check_equal(tf_net_server_port(&server, &port), SALTS_OK);
    check_true(port != 0u);
    client = flowie_test_connect(port);
    check_true(client != FLOWIE_TEST_INVALID_SOCKET);
    check_equal(flowie_test_send(client, inbound, sizeof(inbound)), SALTS_OK);
    check_equal(flow_net_runtime_wait(&probe.opened), SALTS_OK);
    check_equal(flow_net_runtime_wait(&probe.received), SALTS_OK);
    check_equal(probe.payload_size, sizeof(inbound));
    check_equal(memcmp(probe.payload, inbound, sizeof(inbound)), 0);

    connection.slot = atomic_load_explicit(&probe.slot, memory_order_relaxed);
    connection.generation = atomic_load_explicit(&probe.generation, memory_order_relaxed);
    check_equal(tf_net_server_send(&server, connection, outbound, sizeof(outbound)), SALTS_OK);
    check_equal(flowie_test_recv_exact(client, received, sizeof(received)), SALTS_OK);
    check_equal(memcmp(received, outbound, sizeof(outbound)), 0);

    check_equal(tf_net_server_close(&server, connection, FLOW_NET_RUNTIME_TEST_CLOSE_STATUS),
                SALTS_OK);
    check_equal(flow_net_runtime_wait(&probe.closed), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.close_status, memory_order_relaxed),
                FLOW_NET_RUNTIME_TEST_CLOSE_STATUS);
    check_equal(atomic_load_explicit(&probe.close_count, memory_order_relaxed), 1);

    flowie_test_socket_close(client);
    check_equal(tf_net_server_stop(&server, FLOW_NET_RUNTIME_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(tf_net_server_destroy(&server), SALTS_OK);
  }


  it("uses the same Flowie connection contract for UDP") {
    flow_net_runtime_packet_round_trip(TF_NET_TRANSPORT_UDP, CNET_PACKET_UDP, 0u);
  }

  it("uses the same Flowie connection contract for KCP") {
    flow_net_runtime_packet_round_trip(TF_NET_TRANSPORT_KCP, CNET_PACKET_KCP,
                                       UINT32_C(0x12345678));
  }

  it("routes WS through CHTTP WebSocket with the same Flowie connection contract") {
    static const unsigned char inbound[] = "websocket-inbound";
    static const unsigned char outbound[] = "websocket-outbound";
    flow_net_runtime_probe probe = {0};
    tf_net_server server = {0};
    tf_net_server_config server_config =
        flow_net_runtime_config(&probe, TF_NET_TRANSPORT_WS);
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config =
        flow_net_runtime_websocket_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .timeout_ms =
                                                           FLOW_NET_RUNTIME_TEST_TIMEOUT_MS,
                                                       .subprotocol = "mqtt"};
    chttp_websocket_event event = {0};
    tf_net_connection connection;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    server_config.websocket_subprotocol = "mqtt";

    check_equal(tf_net_server_init(&server, &server_config), SALTS_OK);
    check_equal(tf_net_server_start(&server), SALTS_OK);
    check_equal(tf_net_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/mqtt", (unsigned int)port) > 0);
    connect_options.uri = uri;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status),
                SALTS_OK);
    check_equal(http_status, 101u);
    check_equal(flow_net_runtime_wait(&probe.opened), SALTS_OK);
    check_equal(chttp_websocket_client_send_binary(&client, inbound, sizeof(inbound),
                                                   FLOW_NET_RUNTIME_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(flow_net_runtime_wait(&probe.received), SALTS_OK);
    check_equal(probe.payload_size, sizeof(inbound));
    check_equal(memcmp(probe.payload, inbound, sizeof(inbound)), 0);

    connection.slot = atomic_load_explicit(&probe.slot, memory_order_relaxed);
    connection.generation = atomic_load_explicit(&probe.generation, memory_order_relaxed);
    check_equal(tf_net_server_send(&server, connection, outbound, sizeof(outbound)), SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, FLOW_NET_RUNTIME_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.message_type, CHTTP_WEBSOCKET_MESSAGE_BINARY);
    check_equal(event.size, sizeof(outbound));
    check_equal(memcmp(event.data, outbound, sizeof(outbound)), 0);
    check_equal(tf_net_server_close(&server, connection, FLOW_NET_RUNTIME_TEST_CLOSE_STATUS),
                SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, FLOW_NET_RUNTIME_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_CLOSE);
    check_equal(flow_net_runtime_wait(&probe.closed), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.close_status, memory_order_relaxed),
                FLOW_NET_RUNTIME_TEST_CLOSE_STATUS);
    check_equal(atomic_load_explicit(&probe.close_count, memory_order_relaxed), 1);

    check_equal(chttp_websocket_client_destroy(&client, FLOW_NET_RUNTIME_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(tf_net_server_stop(&server, FLOW_NET_RUNTIME_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(tf_net_server_destroy(&server), SALTS_OK);
  }
}
