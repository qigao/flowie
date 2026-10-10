#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "flowie_connection.h"

#include <http_client/http.h>
#include <cnet/owner_placement.h>

#include "../../flowie/tests/flowie_test_cnet.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/error_codes.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#define FLOWIE_CONNECTION_TEST_TIMEOUT_MS 3000u
#define FLOWIE_CONNECTION_TEST_CLOSE_STATUS SALTS_ECANCELED

typedef struct flowie_connection_test_probe {
  atomic_uint slot;
  atomic_uint generation;
  atomic_int opened;
  atomic_int received;
  atomic_int closed;
  atomic_int close_count;
  atomic_int close_status;
  unsigned char payload[64];
  size_t payload_size;
} flowie_connection_test_probe;

typedef struct flow_net_packet_probe {
  atomic_int received;
  unsigned char payload[64];
  size_t payload_size;
} flow_net_packet_probe;

static native_io_backend_kind flowie_connection_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config flowie_connection_test_stream_config(void) {
  const cnet_client_config config = {.backend = flowie_connection_test_backend(),
                                     .connection_capacity = 4u,
                                     .command_capacity = 16u,
                                     .request_capacity = 8u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 16u,
                                     .max_send_bytes = 8192u,
                                     .receive_buffer_bytes = 8192u};
  return config;
}

static chttp_websocket_client_config flowie_connection_test_websocket_client_config(void) {
  const chttp_websocket_client_config config = {.size = sizeof(config),
                                                .network =
                                                    flowie_connection_test_stream_config(),
                                                .max_frame_bytes = 4096u,
                                                .max_message_bytes = 4096u,
                                                .max_buffered_input_bytes = 8192u,
                                                .max_handshake_header_bytes = 4096u,
                                                .event_capacity = 8u};
  return config;
}

static int flowie_connection_test_open(void *user, flowie_connection connection,
                                 const flowie_peer_info *peer) {
  flowie_connection_test_probe *probe = (flowie_connection_test_probe *)user;
  if (probe == NULL || peer == NULL) return SALTS_EINVAL;
  atomic_store_explicit(&probe->slot, connection.slot, memory_order_relaxed);
  atomic_store_explicit(&probe->generation, connection.generation, memory_order_relaxed);
  atomic_store_explicit(&probe->opened, 1, memory_order_release);
  return SALTS_OK;
}

static int flowie_connection_test_receive(void *user, flowie_connection connection, const void *data,
                                    size_t size) {
  flowie_connection_test_probe *probe = (flowie_connection_test_probe *)user;
  (void)connection;
  if (probe == NULL || data == NULL || size > sizeof(probe->payload)) return SALTS_EINVAL;
  memcpy(probe->payload, data, size);
  probe->payload_size = size;
  atomic_store_explicit(&probe->received, 1, memory_order_release);
  return SALTS_OK;
}

static void flowie_connection_test_close(void *user, flowie_connection connection, int status) {
  flowie_connection_test_probe *probe = (flowie_connection_test_probe *)user;
  (void)connection;
  if (probe == NULL) return;
  atomic_store_explicit(&probe->close_status, status, memory_order_relaxed);
  atomic_fetch_add_explicit(&probe->close_count, 1, memory_order_relaxed);
  atomic_store_explicit(&probe->closed, 1, memory_order_release);
}

static int flowie_connection_test_wait(atomic_int *value) {
  const uint64_t deadline = cmeta_monotonic_ms() + FLOWIE_CONNECTION_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) == 0 &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1u);
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
  config.datagram.backend = flowie_connection_test_backend();
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

static flowie_server_config flowie_connection_test_config(flowie_connection_test_probe *probe,
                                                    flowie_transport transport) {
  flowie_server_config config = TF_NET_SERVER_CONFIG_INIT;
  config.transport = transport;
  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = 4u;
  config.path = "/mqtt";
  config.stream = flowie_connection_test_stream_config();
  config.packet = flow_net_packet_config(
      transport == TF_NET_TRANSPORT_UDP ? CNET_PACKET_UDP : CNET_PACKET_KCP, NULL);
  config.command_capacity = 8u;
  config.command_bytes_capacity = 8192u;
  config.max_message_bytes = 4096u;
  config.poll_slice_ms = 1u;
  config.observer.on_open = flowie_connection_test_open;
  config.observer.on_receive = flowie_connection_test_receive;
  config.observer.on_close = flowie_connection_test_close;
  config.observer.user = probe;
  return config;
}

static void flowie_connection_test_packet_round_trip(flowie_transport transport,
                                               cnet_packet_protocol protocol,
                                               uint32_t conversation) {
  static const unsigned char inbound[] = "packet-inbound";
  static const unsigned char outbound[] = "packet-outbound";
  flowie_connection_test_probe server_probe = {0};
  flow_net_packet_probe client_probe = {0};
  flowie_server server = {0};
  flowie_server_config server_config = flowie_connection_test_config(&server_probe, transport);
  cnet_packet_endpoint client = {0};
  cnet_packet_endpoint_config client_config = flow_net_packet_config(protocol, &client_probe);
  cnet_packet_session session = {0};
  cnet_datagram_peer peer = {0};
  flowie_connection connection;
  uint16_t port = 0u;
  size_t events = 0u;
  size_t attempts;

  check_equal(flowie_server_init(&server, &server_config), SALTS_OK);
  check_equal(flowie_server_start(&server), SALTS_OK);
  check_equal(flowie_server_port(&server, &port), SALTS_OK);
  check_true(port != 0u);
  check_equal(cnet_packet_endpoint_init(&client, &client_config), SALTS_OK);
  peer.family = CNET_DATAGRAM_ADDRESS_IPV4;
  peer.port = port;
  peer.address[0] = 127u;
  peer.address[3] = 1u;
  check_equal(cnet_packet_session_open(&client, &peer, conversation, &session), SALTS_OK);
  check_equal(cnet_packet_send(&client, session, inbound, sizeof(inbound)), SALTS_OK);
  for (attempts = 0u; attempts < FLOWIE_CONNECTION_TEST_TIMEOUT_MS &&
                      atomic_load_explicit(&server_probe.received, memory_order_acquire) == 0;
       ++attempts)
    check_equal(cnet_packet_poll(&client, 1u, &events), SALTS_OK);
  check_equal(flowie_connection_test_wait(&server_probe.opened), SALTS_OK);
  check_equal(flowie_connection_test_wait(&server_probe.received), SALTS_OK);
  check_equal(server_probe.payload_size, sizeof(inbound));
  check_equal(memcmp(server_probe.payload, inbound, sizeof(inbound)), 0);

  connection.slot = atomic_load_explicit(&server_probe.slot, memory_order_relaxed);
  connection.generation = atomic_load_explicit(&server_probe.generation, memory_order_relaxed);
  check_equal(flowie_server_send(&server, connection, outbound, sizeof(outbound)), SALTS_OK);
  for (attempts = 0u; attempts < FLOWIE_CONNECTION_TEST_TIMEOUT_MS &&
                      atomic_load_explicit(&client_probe.received, memory_order_acquire) == 0;
       ++attempts)
    check_equal(cnet_packet_poll(&client, 1u, &events), SALTS_OK);
  check_equal(flowie_connection_test_wait(&client_probe.received), SALTS_OK);
  check_equal(client_probe.payload_size, sizeof(outbound));
  check_equal(memcmp(client_probe.payload, outbound, sizeof(outbound)), 0);

  check_equal(flowie_server_close(&server, connection, FLOWIE_CONNECTION_TEST_CLOSE_STATUS),
              SALTS_OK);
  check_equal(flowie_connection_test_wait(&server_probe.closed), SALTS_OK);
  check_equal(atomic_load_explicit(&server_probe.close_status, memory_order_relaxed),
              FLOWIE_CONNECTION_TEST_CLOSE_STATUS);
  check_equal(atomic_load_explicit(&server_probe.close_count, memory_order_relaxed), 1);

  check_equal(flowie_server_stop(&server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(flowie_server_destroy(&server), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&client, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&client), SALTS_OK);
}

#if defined(_WIN32) || defined(__linux__)
/* Select from this test process's allowed set; never assume CPU zero is available. */
static uint32_t flowie_test_allowed_cpu(int require_single, uint32_t ordinal) {
#if defined(_WIN32)
  GROUP_AFFINITY affinity = {0};
  if (!GetThreadGroupAffinity(GetCurrentThread(), &affinity) || affinity.Mask == 0u)
    return UINT32_MAX;
  if (require_single && (affinity.Mask & (affinity.Mask - 1u)) != 0u) return UINT32_MAX;
  for (uint32_t bit = 0u; bit < sizeof(KAFFINITY) * 8u; ++bit)
    if (affinity.Mask & ((KAFFINITY)1u << bit)) {
      if (ordinal == 0u) return affinity.Group * 64u + bit;
      --ordinal;
    }
#else
  cpu_set_t mask;
  if (sched_getaffinity(0, sizeof(mask), &mask) != 0) return UINT32_MAX;
  if (require_single && CPU_COUNT(&mask) != 1) return UINT32_MAX;
  for (uint32_t cpu = 0u; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &mask)) {
      if (ordinal == 0u) return cpu;
      --ordinal;
    }
#endif
  return UINT32_MAX;
}
#endif

typedef struct flowie_multi_probe {
  flowie_server server;
  atomic_int opened;
  atomic_int closed;
  atomic_int error;
  atomic_uint hold_slot;
  atomic_int blocked;
  atomic_int released;
  unsigned char retained[8];
  atomic_uint generations[4];
  atomic_uintptr_t threads[4];
  int check_affinity;
  uint32_t expected_cpus[4];
} flowie_multi_probe;

static int flowie_multi_open(void *user, flowie_connection connection,
                              const flowie_peer_info *peer) {
  flowie_multi_probe *probe = (flowie_multi_probe *)user;
  if (peer == NULL || connection.slot == 0u || connection.slot > 4u) {
    atomic_store(&probe->error, SALTS_ERANGE);
    return SALTS_ERANGE;
  }
  atomic_store(&probe->generations[connection.slot - 1u], connection.generation);
  atomic_store(&probe->threads[connection.slot - 1u],
               (uintptr_t)cmeta_thread_current_token());
#if defined(_WIN32) || defined(__linux__)
  if (probe->check_affinity &&
      flowie_test_allowed_cpu(1, 0u) != probe->expected_cpus[connection.slot - 1u])
    atomic_store(&probe->error, SALTS_EINVAL);
#endif
  atomic_fetch_add(&probe->opened, 1);
  return SALTS_OK;
}

static int flowie_multi_receive(void *user, flowie_connection connection,
                                 const void *data, size_t size) {
  flowie_multi_probe *probe = (flowie_multi_probe *)user;
  mem_buffer_t *buffer;
  mem_slice_t slices[2];
  int status;
  if (connection.slot == 0u || connection.slot > 4u ||
      atomic_load(&probe->threads[connection.slot - 1u]) !=
          (uintptr_t)cmeta_thread_current_token()) {
    atomic_store(&probe->error, SALTS_EINVAL);
    return SALTS_EINVAL;
  }
  if (atomic_load(&probe->hold_slot) == connection.slot) {
    const uint64_t deadline = cmeta_monotonic_ms() + FLOWIE_CONNECTION_TEST_TIMEOUT_MS;
    atomic_store(&probe->blocked, 1);
    while (atomic_load(&probe->hold_slot) == connection.slot && cmeta_monotonic_ms() < deadline)
      cmeta_sleep_ms(1u);
    if (atomic_load(&probe->hold_slot) == connection.slot) return SALTS_ETIMEDOUT;
    return SALTS_OK;
  }
#if defined(_WIN32) || defined(__linux__)
  if (probe->check_affinity &&
      flowie_test_allowed_cpu(1, 0u) != probe->expected_cpus[connection.slot - 1u])
    atomic_store(&probe->error, SALTS_EINVAL);
#endif
  buffer = mem_get_buffer(mem_global(), size);
  if (buffer == NULL) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), data, size);
  mem_set_used(buffer, size);
  slices[0] = mem_slice(buffer, 0u, size > 1u ? size / 2u : size);
  slices[1] = size > 1u ? mem_slice(buffer, size / 2u, size - size / 2u) : (mem_slice_t){0};
  status = flowie_server_send_slicev(&probe->server, connection, slices, size > 1u ? 2u : 1u);
  mem_slice_release(&slices[0]);
  mem_slice_release(&slices[1]);
  mem_buffer_release(buffer);
  if (status != SALTS_OK) atomic_store(&probe->error, status);
  return status;
}

static void flowie_multi_close(void *user, flowie_connection connection, int status) {
  flowie_multi_probe *probe = (flowie_multi_probe *)user;
  (void)status;
  if (connection.slot == 0u || connection.slot > 4u ||
      atomic_load(&probe->threads[connection.slot - 1u]) !=
          (uintptr_t)cmeta_thread_current_token()) {
    atomic_store(&probe->error, SALTS_EINVAL);
  } else {
#if defined(_WIN32) || defined(__linux__)
    if (probe->check_affinity &&
        flowie_test_allowed_cpu(1, 0u) != probe->expected_cpus[connection.slot - 1u])
      atomic_store(&probe->error, SALTS_EINVAL);
#endif
  }
  atomic_fetch_add(&probe->closed, 1);
}

static int flowie_multi_wait(atomic_int *value, int expected) {
  const uint64_t deadline = cmeta_monotonic_ms() + FLOWIE_CONNECTION_TEST_TIMEOUT_MS;
  while (atomic_load(value) < expected && cmeta_monotonic_ms() < deadline) cmeta_sleep_ms(1u);
  return atomic_load(value) == expected ? SALTS_OK : SALTS_ETIMEDOUT;
}

static void flowie_multi_released(void *data, void *user) {
  (void)data;
  atomic_fetch_add((atomic_int *)user, 1);
}

static int flowie_multi_queue_retained(flowie_multi_probe *probe, flowie_connection connection) {
  mem_buffer_t *buffer = mem_wrap_external(probe->retained, sizeof(probe->retained),
                                           flowie_multi_released, &probe->released);
  mem_slice_t slice;
  int status;
  if (buffer == NULL) return SALTS_ENOMEM;
  slice = mem_slice(buffer, 0u, sizeof(probe->retained));
  status = flowie_server_send_slicev(&probe->server, connection, &slice, 1u);
  mem_slice_release(&slice);
  mem_buffer_release(buffer);
  return status;
}

spec("Flowie multiple network owners") {
  static flowie_multi_probe probe;
  static flowie_test_cnet_client_t *clients[4];

  before_each() {
    memset(&probe, 0, sizeof(probe));
    memset(clients, 0, sizeof(clients));
  }
  after_each() {
    atomic_store(&probe.hold_slot, 0u);
    if (probe.server.impl != NULL) {
      (void)flowie_server_stop(&probe.server, 0u);
      (void)flowie_server_destroy(&probe.server);
    }
    for (size_t index = 0u; index < 4u; ++index) flowie_test_cnet_close(clients[index]);
  }

  it("recycles bounded manager records and handoff credits across repeated generations") {
    for (uint32_t owners = 1u; owners <= 2u; ++owners) {
      flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
      flowie_connection stale[2] = {{0}};
      uint16_t port = 0u;
      unsigned char received[4];
      memset(&probe, 0, sizeof(probe));
      config.network_workers = owners;
      config.stream.connection_capacity = owners;
      config.observer = (flowie_observer){flowie_multi_open, flowie_multi_receive,
                                          flowie_multi_close, NULL, &probe};
      check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
      check_equal(flowie_server_start(&probe.server), SALTS_OK);
      check_equal(flowie_server_port(&probe.server, &port), SALTS_OK);
      for (int round = 0; round < 32; ++round) {
        for (uint32_t index = 0u; index < owners; ++index) {
          clients[index] = flowie_test_cnet_connect(port);
          check_not_null(clients[index]);
          check_equal(flowie_multi_wait(&probe.opened, round * (int)owners + (int)index + 1), SALTS_OK);
        }
        for (uint32_t index = 0u; index < owners; ++index) {
          const flowie_connection current = {index + 1u, atomic_load(&probe.generations[index])};
          if (round != 0) {
            check_not_equal(current.generation, stale[index].generation);
            check_equal(flowie_server_close(&probe.server, stale[index], SALTS_ECANCELED), SALTS_OK);
            check_equal(flowie_server_send(&probe.server, stale[index], "old!", 4u), SALTS_OK);
          }
          check_equal(flowie_server_send(&probe.server, current, "live", 4u), SALTS_OK);
          check_equal(flowie_test_cnet_recv_exact(clients[index], received, 4u), SALTS_OK);
          check_equal(memcmp(received, "live", 4u), 0);
          stale[index] = current;
          check_equal(flowie_server_close(&probe.server, current, SALTS_ECANCELED), SALTS_OK);
        }
        check_equal(flowie_multi_wait(&probe.closed, (round + 1) * (int)owners), SALTS_OK);
        for (uint32_t index = 0u; index < owners; ++index) {
          flowie_test_cnet_close(clients[index]);
          clients[index] = NULL;
        }
      }
      check_equal(flowie_server_stop(&probe.server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(atomic_load(&probe.error), SALTS_OK);
      check_equal(flowie_server_destroy(&probe.server), SALTS_OK);
    }
  }

  it("keeps four fixed CNet Owners distinct and drains all live connections") {
    for (uint32_t cycle = 0u; cycle < 4u; ++cycle) {
      flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
      uint16_t port = 0u;
      unsigned char received[4] = {0};
      memset(&probe, 0, sizeof(probe));
      config.network_workers = 4u;
      config.stream.connection_capacity = 4u;
      config.command_bytes_capacity = 4u * config.max_message_bytes;
      config.observer = (flowie_observer){flowie_multi_open, flowie_multi_receive,
                                          flowie_multi_close, NULL, &probe};
      check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
      check_equal(flowie_server_start(&probe.server), SALTS_OK);
      check_equal(flowie_server_port(&probe.server, &port), SALTS_OK);
      for (uint32_t index = 0u; index < 4u; ++index) {
        unsigned char payload[4] = {(unsigned char)index, (unsigned char)cycle, 0x45u, 0x6au};
        clients[index] = flowie_test_cnet_connect(port);
        check_not_null(clients[index]);
        check_equal(flowie_multi_wait(&probe.opened, (int)(index + 1u)), SALTS_OK);
        check_equal(flowie_test_cnet_send(clients[index], payload, sizeof(payload)), SALTS_OK);
        check_equal(flowie_test_cnet_recv_exact(clients[index], received, sizeof(received)),
                    SALTS_OK);
        check_equal(memcmp(received, payload, sizeof(payload)), 0);
      }
      for (uint32_t index = 0u; index < 4u; ++index) {
        check_not_equal(atomic_load(&probe.threads[index]), (uintptr_t)0u);
        for (uint32_t previous = 0u; previous < index; ++previous)
          check_not_equal(atomic_load(&probe.threads[index]),
                          atomic_load(&probe.threads[previous]));
      }
      check_equal(flowie_server_stop(&probe.server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS),
                  SALTS_OK);
      check_equal(atomic_load(&probe.closed), 4);
      check_equal(atomic_load(&probe.error), SALTS_OK);
      check_equal(flowie_server_destroy(&probe.server), SALTS_OK);
      for (size_t index = 0u; index < 4u; ++index) {
        flowie_test_cnet_close(clients[index]);
        clients[index] = NULL;
      }
    }
  }

  it("quiesces an idle four-Owner acceptor before native worker destruction") {
    for (uint32_t cycle = 0u; cycle < 8u; ++cycle) {
      flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
      memset(&probe, 0, sizeof(probe));
      config.network_workers = 4u;
      config.stream.connection_capacity = 4u;
      config.command_bytes_capacity = 4u * config.max_message_bytes;
      config.observer = (flowie_observer){flowie_multi_open, flowie_multi_receive,
                                          flowie_multi_close, NULL, &probe};
      check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
      check_equal(flowie_server_start(&probe.server), SALTS_OK);
      check_equal(flowie_server_stop(&probe.server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS),
                  SALTS_OK);
      check_equal(atomic_load(&probe.opened), 0);
      check_equal(atomic_load(&probe.closed), 0);
      check_equal(flowie_server_destroy(&probe.server), SALTS_OK);
    }
  }

  it("places new connections on the least occupied owner without migrating live connections") {
    flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
    flowie_connection closed;
    uintptr_t first_thread;
    uint16_t port = 0u;
    unsigned char received[4];
    config.network_workers = 2u;
    config.network_policy = TF_NET_OWNER_LEAST_CONNECTIONS;
    config.stream.connection_capacity = 3u;
    config.observer = (flowie_observer){flowie_multi_open, flowie_multi_receive,
                                       flowie_multi_close, NULL, &probe};
    check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
    check_equal(flowie_server_start(&probe.server), SALTS_OK);
    check_equal(flowie_server_port(&probe.server, &port), SALTS_OK);
    for (size_t index = 0u; index < 2u; ++index) {
      clients[index] = flowie_test_cnet_connect(port);
      check_not_null(clients[index]);
      check_equal(flowie_multi_wait(&probe.opened, (int)index + 1), SALTS_OK);
    }
    first_thread = atomic_load(&probe.threads[0]);
    check_not_equal(first_thread, atomic_load(&probe.threads[1]));
    closed = (flowie_connection){2u, atomic_load(&probe.generations[1])};
    check_equal(flowie_server_close(&probe.server, closed, SALTS_ECANCELED), SALTS_OK);
    check_equal(flowie_multi_wait(&probe.closed, 1), SALTS_OK);
    /* Round-robin would choose owner 0/slot 3 here; least-connections reuses slot 2. */
    clients[2] = flowie_test_cnet_connect(port);
    check_not_null(clients[2]);
    check_equal(flowie_multi_wait(&probe.opened, 3), SALTS_OK);
    check_not_equal(atomic_load(&probe.generations[1]), closed.generation);
    check_equal(atomic_load(&probe.generations[2]), 0u);
    check_equal(flowie_test_cnet_send(clients[0], (const uint8_t *)"live", 4u), SALTS_OK);
    check_equal(flowie_test_cnet_recv_exact(clients[0], received, 4u), SALTS_OK);
    check_equal(memcmp(received, "live", 4u), 0);
    check_equal(atomic_load(&probe.threads[0]), first_thread);
    check_equal(atomic_load(&probe.error), SALTS_OK);
  }

#if defined(_WIN32) || defined(__linux__)
  it("binds each owner before callbacks and copies the configured CPU list") {
    flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
    uint16_t port = 0u;
    const uint32_t cpu = flowie_test_allowed_cpu(0, 0u);
    const uint32_t second_cpu = flowie_test_allowed_cpu(0, 1u);
    unsigned char received[4];
    check_not_equal(cpu, UINT32_MAX);
    config.network_workers = 2u;
    config.stream.connection_capacity = 2u;
    config.network_cpu_count = 2u;
    config.network_cpus[0] = cpu;
    /* A single-CPU process can still run two owners; use distinct CPUs when allowed. */
    config.network_cpus[1] = second_cpu == UINT32_MAX ? cpu : second_cpu;
    config.observer = (flowie_observer){flowie_multi_open, flowie_multi_receive,
                                       flowie_multi_close, NULL, &probe};
    probe.check_affinity = 1;
    probe.expected_cpus[0] = config.network_cpus[0];
    probe.expected_cpus[1] = config.network_cpus[1];
    check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
    config.network_cpus[0] = config.network_cpus[1] = UINT32_MAX;
    check_equal(flowie_server_start(&probe.server), SALTS_OK);
    check_equal(flowie_server_port(&probe.server, &port), SALTS_OK);
    for (size_t index = 0u; index < 2u; ++index) {
      clients[index] = flowie_test_cnet_connect(port);
      check_not_null(clients[index]);
      check_equal(flowie_multi_wait(&probe.opened, (int)index + 1), SALTS_OK);
      check_equal(flowie_test_cnet_send(clients[index], (const uint8_t *)"pins", 4u), SALTS_OK);
      check_equal(flowie_test_cnet_recv_exact(clients[index], received, sizeof(received)), SALTS_OK);
      check_equal(memcmp(received, "pins", sizeof(received)), 0);
    }
    check_not_equal(atomic_load(&probe.threads[0]), atomic_load(&probe.threads[1]));
    check_equal(flowie_server_stop(&probe.server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(atomic_load(&probe.closed), 2);
    check_equal(atomic_load(&probe.error), SALTS_OK);
  }

  it("joins started owners when a later owner cannot bind its CPU") {
    flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
    config.network_workers = 2u;
    config.network_cpu_count = 2u;
    config.network_cpus[0] = flowie_test_allowed_cpu(0, 0u);
    check_not_equal(config.network_cpus[0], UINT32_MAX);
    config.network_cpus[1] = UINT32_MAX;
    config.observer = (flowie_observer){flowie_multi_open, flowie_multi_receive,
                                       flowie_multi_close, NULL, &probe};
    check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
    check_equal(flowie_server_start(&probe.server), SALTS_ERANGE);
    check_equal(atomic_load(&probe.opened), 0);
    check_equal(flowie_server_start(&probe.server), SALTS_ESHUTDOWN);
    check_equal(flowie_server_destroy(&probe.server), SALTS_OK);
    check_null(probe.server.impl);
    config.network_workers = config.network_cpu_count = 1u;
    config.network_cpus[0] = UINT32_MAX;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
    check_equal(flowie_server_start(&probe.server), SALTS_ERANGE);
    check_equal(flowie_server_destroy(&probe.server), SALTS_OK);
  }
#endif

  it("validates owner policy and CPU list before allocating owners") {
    flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
    config.network_workers = 2u;
    config.network_cpu_count = 1u;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_EINVAL);
    config.network_cpu_count = FLOWIE_NETWORK_WORKERS_MAX + 1u;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_EINVAL);
    config.network_cpu_count = 0u;
    config.network_policy = (flowie_owner_policy)CNET_OWNER_PLACE_ROUND_ROBIN;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_EINVAL);
    config.network_policy = (flowie_owner_policy)CNET_OWNER_PLACE_STRICT_KEY;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_EINVAL);
    config.network_workers = 1u;
    config.transport = TF_NET_TRANSPORT_UDP;
    config.network_policy = TF_NET_OWNER_LEAST_CONNECTIONS;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_ENOTSUP);
    config.network_policy = TF_NET_OWNER_ROUND_ROBIN;
    config.network_cpu_count = 1u;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_ENOTSUP);
    check_null(probe.server.impl);
  }

  it("progresses another owner while one callback blocks and preserves timed-out stop ownership") {
    flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
    uint16_t port = 0u;
    unsigned char received[8];
    config.network_workers = 2u;
    config.stream.connection_capacity = 2u;
    config.observer = (flowie_observer){flowie_multi_open, flowie_multi_receive,
                                        flowie_multi_close, NULL, &probe};
    check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
    check_equal(flowie_server_stop(&probe.server, 1u), SALTS_OK);
    check_equal(flowie_server_start(&probe.server), SALTS_OK);
    check_equal(flowie_server_port(&probe.server, &port), SALTS_OK);
    for (size_t index = 0u; index < 2u; ++index) {
      clients[index] = flowie_test_cnet_connect(port);
      check_not_null(clients[index]);
      check_equal(flowie_multi_wait(&probe.opened, (int)index + 1), SALTS_OK);
    }
    atomic_store(&probe.hold_slot, 1u);
    check_equal(flowie_test_cnet_send(clients[0], "blocked!", sizeof(received)), SALTS_OK);
    check_equal(flowie_multi_wait(&probe.blocked, 1), SALTS_OK);
    {
      const flowie_connection blocked = {1u, atomic_load(&probe.generations[0])};
      check_equal(flowie_multi_queue_retained(&probe, blocked), SALTS_OK);
      for (size_t index = 1u; index < config.command_capacity; ++index)
        check_equal(flowie_server_send(&probe.server, blocked, "queued!!", sizeof(received)), SALTS_OK);
      check_equal(flowie_server_send(&probe.server, blocked, "overflow", sizeof(received)), SALTS_ENOBUFS);
      check_equal(atomic_load(&probe.released), 0);
    }
    check_equal(flowie_test_cnet_send(clients[1], "running!", sizeof(received)), SALTS_OK);
    check_equal(flowie_test_cnet_recv_exact(clients[1], received, sizeof(received)), SALTS_OK);
    check_equal(memcmp(received, "running!", sizeof(received)), 0);
    check_equal(flowie_server_stop(&probe.server, 1u), SALTS_ETIMEDOUT);
    check_equal(flowie_server_destroy(&probe.server), SALTS_EBUSY);
    atomic_store(&probe.hold_slot, 0u);
    check_equal(flowie_server_stop(&probe.server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(atomic_load(&probe.closed), 2);
    check_equal(atomic_load(&probe.error), SALTS_OK);
    check_equal(flowie_server_destroy(&probe.server), SALTS_OK);
    check_equal(atomic_load(&probe.released), 1);
  }

  it("routes retained SG and copied sends to fixed owners and fences recycled generations") {
    flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_TCP);
    flowie_connection old;
    uint16_t port = 0u;
    unsigned char received[8];
    static const unsigned char stale[] = "stale!!!";
    config.network_workers = 2u;
    config.stream.connection_capacity = 3u; /* Uneven partition: 2 + 1, not 2 + 2. */
    config.observer = (flowie_observer){flowie_multi_open, flowie_multi_receive,
                                        flowie_multi_close, NULL, &probe};
    check_equal(flowie_server_init(&probe.server, &config), SALTS_OK);
    check_equal(flowie_server_start(&probe.server), SALTS_OK);
    check_equal(flowie_server_port(&probe.server, &port), SALTS_OK);
    for (size_t index = 0u; index < 3u; ++index) {
      clients[index] = flowie_test_cnet_connect(port);
      check_not_null(clients[index]);
      check_equal(flowie_multi_wait(&probe.opened, (int)index + 1), SALTS_OK);
    }
    check_not_equal(atomic_load(&probe.threads[0]), atomic_load(&probe.threads[1]));
    check_equal(atomic_load(&probe.threads[0]), atomic_load(&probe.threads[2]));
    clients[3] = flowie_test_cnet_connect(port);
    check_not_null(clients[3]);
    for (size_t round = 0u; round < 4u; ++round) {
      for (size_t index = 0u; index < 3u; ++index) {
        unsigned char payload[8];
        memset(payload, (int)(index + round * 3u), sizeof(payload));
        check_equal(flowie_test_cnet_send(clients[index], payload, sizeof(payload)), SALTS_OK);
        check_equal(flowie_test_cnet_recv_exact(clients[index], received, sizeof(received)), SALTS_OK);
        check_equal(memcmp(received, payload, sizeof(payload)), 0);
        check_equal(flowie_server_send(&probe.server,
                        (flowie_connection){(uint32_t)index + 1u,
                          atomic_load(&probe.generations[index])}, payload, sizeof(payload)), SALTS_OK);
        check_equal(flowie_test_cnet_recv_exact(clients[index], received, sizeof(received)), SALTS_OK);
        check_equal(memcmp(received, payload, sizeof(payload)), 0);
      }
    }
    check_equal(atomic_load(&probe.opened), 3);
    old = (flowie_connection){2u, atomic_load(&probe.generations[1])};
    check_equal(flowie_server_close(&probe.server, old, SALTS_ECANCELED), SALTS_OK);
    check_equal(flowie_multi_wait(&probe.closed, 1), SALTS_OK);
    check_equal(flowie_multi_wait(&probe.opened, 4), SALTS_OK);
    check_not_equal(atomic_load(&probe.generations[1]), old.generation);
    /* Old admission may be queued, but must never reach the recycled socket. */
    check_equal(flowie_server_send(&probe.server, old, stale, sizeof(received)), SALTS_OK);
    check_equal(flowie_server_send(&probe.server,
                    (flowie_connection){2u, atomic_load(&probe.generations[1])},
                    "current!", sizeof(received)), SALTS_OK);
    check_equal(flowie_test_cnet_recv_exact(clients[3], received, sizeof(received)), SALTS_OK);
    check_equal(memcmp(received, "current!", sizeof(received)), 0);
    check_equal(flowie_server_stop(&probe.server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(atomic_load(&probe.closed), 4);
    check_equal(atomic_load(&probe.error), SALTS_OK);
    check_equal(flowie_server_send(&probe.server, old, stale, sizeof(received)), SALTS_ESHUTDOWN);
  }

  it("rejects unsupported transports and undersized aggregate partitions") {
    flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_UDP);
    config.network_workers = 2u;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_ENOTSUP);
    config.transport = TF_NET_TRANSPORT_TCP;
    config.command_bytes_capacity = config.max_message_bytes;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_ERANGE);
    config.command_bytes_capacity *= 2u;
    config.network_workers = 5u;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_ERANGE);
    config.network_workers = FLOWIE_NETWORK_WORKERS_MAX + 1u;
    check_equal(flowie_server_init(&probe.server, &config), SALTS_ERANGE);
    check_null(probe.server.impl);
  }
}


typedef struct flowie_ws_multi_owner_probe {
  flowie_server *server;
  atomic_int opened;
  atomic_int closed;
  atomic_int errors;
  atomic_uintptr_t callback_threads[2];
} flowie_ws_multi_owner_probe;

static int flowie_ws_multi_open(void *user, flowie_connection connection,
                                const flowie_peer_info *peer) {
  flowie_ws_multi_owner_probe *probe = (flowie_ws_multi_owner_probe *)user;
  int index;
  (void)connection;
  if (peer == NULL) return SALTS_EINVAL;
  index = atomic_fetch_add(&probe->opened, 1);
  if (index >= 2) return SALTS_ENOBUFS;
  atomic_store(&probe->callback_threads[index],
               (uintptr_t)cmeta_thread_current_token());
  return SALTS_OK;
}

static int flowie_ws_multi_receive(void *user, flowie_connection connection,
                                   const void *data, size_t size) {
  flowie_ws_multi_owner_probe *probe = (flowie_ws_multi_owner_probe *)user;
  const int status = flowie_server_send(probe->server, connection, data, size);
  if (status != SALTS_OK) atomic_store(&probe->errors, status);
  return status;
}

static void flowie_ws_multi_close(void *user, flowie_connection connection,
                                  int status) {
  flowie_ws_multi_owner_probe *probe = (flowie_ws_multi_owner_probe *)user;
  (void)connection;
  (void)status;
  atomic_fetch_add(&probe->closed, 1);
}

spec("Flowie CNet and CHTTP transport connection") {
  it("formats copied IPv4 peer metadata without transport-owned pointers") {
    flowie_peer_info peer = {0};
    char text[TF_NET_PEER_TEXT_CAPACITY];
    peer.peer.family = CNET_DATAGRAM_ADDRESS_IPV4;
    peer.peer.port = 1883u;
    peer.peer.address[0] = 127u;
    peer.peer.address[3] = 1u;
    check_equal(flowie_peer_format(&peer, text, sizeof(text)), SALTS_OK);
    check_equal(strcmp(text, "127.0.0.1:1883"), 0);
  }

  it("accepts TCP and admits a copied send from a non-owner thread") {
    static const unsigned char inbound[] = "cnet-inbound";
    static const unsigned char outbound[] = "cnet-outbound";
    unsigned char send_payload[sizeof(outbound)];
    flowie_connection_test_probe probe = {0};
    flowie_server server = {0};
    flowie_server_config config = flowie_connection_test_config(&probe, TF_NET_TRANSPORT_TCP);
    flowie_connection connection;
    flowie_test_cnet_client_t * client = FLOWIE_TEST_INVALID_CNET_CLIENT;
    unsigned char received[sizeof(outbound)] = {0};
    uint16_t port = 0u;

    config.stream_socket_options.receive_buffer_bytes = 32768u;
    config.stream_socket_options.send_buffer_bytes = 32768u;
    config.stream_socket_options.keepalive = 1;
    config.stream_socket_options.linger = 1;

    check_equal(flowie_server_init(&server, &config), SALTS_OK);
    check_equal(flowie_server_start(&server), SALTS_OK);
    check_equal(flowie_server_port(&server, &port), SALTS_OK);
    check_true(port != 0u);
    client = flowie_test_cnet_connect(port);
    check_true(client != FLOWIE_TEST_INVALID_CNET_CLIENT);
    check_equal(flowie_test_cnet_send(client, inbound, sizeof(inbound)), SALTS_OK);
    check_equal(flowie_connection_test_wait(&probe.opened), SALTS_OK);
    check_equal(flowie_connection_test_wait(&probe.received), SALTS_OK);
    check_equal(probe.payload_size, sizeof(inbound));
    check_equal(memcmp(probe.payload, inbound, sizeof(inbound)), 0);

    connection.slot = atomic_load_explicit(&probe.slot, memory_order_relaxed);
    connection.generation = atomic_load_explicit(&probe.generation, memory_order_relaxed);
    memcpy(send_payload, outbound, sizeof(outbound));
    check_equal(flowie_server_send(&server, connection, send_payload, sizeof(send_payload)),
                SALTS_OK);
    memset(send_payload, 'x', sizeof(send_payload));
    check_equal(flowie_server_send(&server, connection, send_payload, sizeof(send_payload)),
                SALTS_OK);
    memset(send_payload, 'y', sizeof(send_payload));
    check_equal(flowie_test_cnet_recv_exact(client, received, sizeof(received)), SALTS_OK);
    check_equal(memcmp(received, outbound, sizeof(outbound)), 0);
    check_equal(flowie_test_cnet_recv_exact(client, received, sizeof(received)), SALTS_OK);
    memset(send_payload, 'x', sizeof(send_payload));
    check_equal(memcmp(received, send_payload, sizeof(send_payload)), 0);

    check_equal(flowie_server_close(&server, connection, FLOWIE_CONNECTION_TEST_CLOSE_STATUS),
                SALTS_OK);
    check_equal(flowie_connection_test_wait(&probe.closed), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.close_status, memory_order_relaxed),
                FLOWIE_CONNECTION_TEST_CLOSE_STATUS);
    check_equal(atomic_load_explicit(&probe.close_count, memory_order_relaxed), 1);

    flowie_test_cnet_close(client);
    check_equal(flowie_server_stop(&server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(flowie_server_destroy(&server), SALTS_OK);
  }


  it("retains TCP scatter gather slices across the non-owner mailbox") {
    static const unsigned char inbound[] = "sg-inbound";
    static const unsigned char first[] = "sg-first";
    static const unsigned char second[] = "sg-second";
    flowie_connection_test_probe probe = {0};
    flowie_server server = {0};
    flowie_server_config config = flowie_connection_test_config(&probe, TF_NET_TRANSPORT_TCP);
    flowie_connection connection;
    flowie_test_cnet_client_t *client = FLOWIE_TEST_INVALID_CNET_CLIENT;
    mem_buffer_t *first_buffer = NULL;
    mem_buffer_t *second_buffer = NULL;
    mem_slice_t slices[2] = {0};
    unsigned char expected[sizeof(first) + sizeof(second)] = {0};
    unsigned char received[sizeof(expected)] = {0};
    uint16_t port = 0u;

    check_equal(flowie_server_init(&server, &config), SALTS_OK);
    check_equal(flowie_server_start(&server), SALTS_OK);
    check_equal(flowie_server_port(&server, &port), SALTS_OK);
    client = flowie_test_cnet_connect(port);
    check_true(client != FLOWIE_TEST_INVALID_CNET_CLIENT);
    check_equal(flowie_test_cnet_send(client, inbound, sizeof(inbound)), SALTS_OK);
    check_equal(flowie_connection_test_wait(&probe.opened), SALTS_OK);
    check_equal(flowie_connection_test_wait(&probe.received), SALTS_OK);

    connection.slot = atomic_load_explicit(&probe.slot, memory_order_relaxed);
    connection.generation = atomic_load_explicit(&probe.generation, memory_order_relaxed);
    first_buffer = mem_get_buffer(mem_global(), sizeof(first));
    second_buffer = mem_get_buffer(mem_global(), sizeof(second));
    check_not_null(first_buffer);
    check_not_null(second_buffer);
    memcpy(mem_buffer_data(first_buffer), first, sizeof(first));
    memcpy(mem_buffer_data(second_buffer), second, sizeof(second));
    mem_set_used(first_buffer, sizeof(first));
    mem_set_used(second_buffer, sizeof(second));
    slices[0] = mem_slice(first_buffer, 0u, sizeof(first));
    slices[1] = mem_slice(second_buffer, 0u, sizeof(second));
    check_not_null(slices[0].buffer);
    check_not_null(slices[1].buffer);

    check_equal(flowie_server_send_slicev(&server, connection, slices, 2u), SALTS_OK);
    mem_slice_release(&slices[0]);
    mem_slice_release(&slices[1]);
    mem_buffer_release(first_buffer);
    mem_buffer_release(second_buffer);
    first_buffer = NULL;
    second_buffer = NULL;

    memcpy(expected, first, sizeof(first));
    memcpy(expected + sizeof(first), second, sizeof(second));
    check_equal(flowie_test_cnet_recv_exact(client, received, sizeof(received)), SALTS_OK);
    check_equal(memcmp(received, expected, sizeof(expected)), 0);

    check_equal(flowie_server_close(&server, connection, FLOWIE_CONNECTION_TEST_CLOSE_STATUS),
                SALTS_OK);
    check_equal(flowie_connection_test_wait(&probe.closed), SALTS_OK);
    flowie_test_cnet_close(client);
    check_equal(flowie_server_stop(&server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(flowie_server_destroy(&server), SALTS_OK);
  }


  it("uses the same Flowie connection contract for UDP") {
    flowie_connection_test_packet_round_trip(TF_NET_TRANSPORT_UDP, CNET_PACKET_UDP, 0u);
  }

  it("uses the same Flowie connection contract for KCP") {
    flowie_connection_test_packet_round_trip(TF_NET_TRANSPORT_KCP, CNET_PACKET_KCP,
                                       UINT32_C(0x12345678));
  }


  it("uses CHttp 2.1 fixed Owners and CNet placement for WS connections") {
    flowie_server server = {0};
    flowie_ws_multi_owner_probe probe = {0};
    flowie_server_config config = flowie_connection_test_config(NULL, TF_NET_TRANSPORT_WS);
    chttp_websocket_client clients[2] = {{0}};
    chttp_websocket_client_config client_config =
        flowie_connection_test_websocket_client_config();
    chttp_websocket_connect_options options = {.size = sizeof(options),
        .timeout_ms = FLOWIE_CONNECTION_TEST_TIMEOUT_MS, .subprotocol = "mqtt"};
    chttp_websocket_event event = {0};
    uint16_t port = 0u;
    char uri[128];
    static const unsigned char payload[] = "two-owner-websocket";
    probe.server = &server;
    config.network_workers = 2u;
    config.network_policy = TF_NET_OWNER_LEAST_CONNECTIONS;
    config.stream.connection_capacity = 2u;
    config.websocket_subprotocol = "mqtt";
    config.observer = (flowie_observer){flowie_ws_multi_open, flowie_ws_multi_receive,
                                        flowie_ws_multi_close, NULL, &probe};
    check_equal(flowie_server_init(&server, &config), SALTS_OK);
    check_equal(flowie_server_start(&server), SALTS_OK);
    check_equal(flowie_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/mqtt",
                        (unsigned int)port) > 0);
    options.uri = uri;
    for (size_t i = 0u; i < 2u; ++i) {
      unsigned int http_status = 0u;
      check_equal(chttp_websocket_client_init(&clients[i], &client_config), SALTS_OK);
      check_equal(chttp_websocket_client_connect(&clients[i], &options, &http_status), SALTS_OK);
      check_equal(http_status, 101u);
      check_equal(chttp_websocket_client_send_binary(
          &clients[i], payload, sizeof(payload), FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_websocket_client_receive(
          &clients[i], FLOWIE_CONNECTION_TEST_TIMEOUT_MS, &event), SALTS_OK);
      check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
      check_equal(event.size, sizeof(payload));
      check_equal(memcmp(event.data, payload, sizeof(payload)), 0);
    }
    check_equal(atomic_load(&probe.opened), 2);
    check_not_equal(atomic_load(&probe.callback_threads[0]), (uintptr_t)0u);
    check_not_equal(atomic_load(&probe.callback_threads[1]), (uintptr_t)0u);
    check_not_equal(atomic_load(&probe.callback_threads[0]),
                    atomic_load(&probe.callback_threads[1]));
    for (size_t i = 0u; i < 2u; ++i)
      check_equal(chttp_websocket_client_destroy(
          &clients[i], FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(flowie_server_stop(&server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(flowie_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load(&probe.errors), SALTS_OK);
  }

  it("routes WS through CHTTP WebSocket with the same Flowie connection contract") {
    static const unsigned char inbound[] = "websocket-inbound";
    static const unsigned char outbound[] = "websocket-outbound";
    flowie_connection_test_probe probe = {0};
    flowie_server server = {0};
    flowie_server_config server_config =
        flowie_connection_test_config(&probe, TF_NET_TRANSPORT_WS);
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config =
        flowie_connection_test_websocket_client_config();
    chttp_websocket_connect_options connect_options = {.size = sizeof(connect_options),
                                                       .timeout_ms =
                                                           FLOWIE_CONNECTION_TEST_TIMEOUT_MS,
                                                       .subprotocol = "mqtt"};
    chttp_websocket_event event = {0};
    flowie_connection connection;
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];

    server_config.websocket_subprotocol = "mqtt";

    check_equal(flowie_server_init(&server, &server_config), SALTS_OK);
    check_equal(flowie_server_start(&server), SALTS_OK);
    check_equal(flowie_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/mqtt", (unsigned int)port) > 0);
    connect_options.uri = uri;
    check_equal(chttp_websocket_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_websocket_client_connect(&client, &connect_options, &http_status),
                SALTS_OK);
    check_equal(http_status, 101u);
    check_equal(flowie_connection_test_wait(&probe.opened), SALTS_OK);
    check_equal(chttp_websocket_client_send_binary(&client, inbound, sizeof(inbound),
                                                   FLOWIE_CONNECTION_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(flowie_connection_test_wait(&probe.received), SALTS_OK);
    check_equal(probe.payload_size, sizeof(inbound));
    check_equal(memcmp(probe.payload, inbound, sizeof(inbound)), 0);

    connection.slot = atomic_load_explicit(&probe.slot, memory_order_relaxed);
    connection.generation = atomic_load_explicit(&probe.generation, memory_order_relaxed);
    check_equal(flowie_server_send(&server, connection, outbound, sizeof(outbound)), SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, FLOWIE_CONNECTION_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal(event.message_type, CHTTP_WEBSOCKET_MESSAGE_BINARY);
    check_equal(event.size, sizeof(outbound));
    check_equal(memcmp(event.data, outbound, sizeof(outbound)), 0);
    check_equal(flowie_server_close(&server, connection, FLOWIE_CONNECTION_TEST_CLOSE_STATUS),
                SALTS_OK);
    check_equal(chttp_websocket_client_receive(&client, FLOWIE_CONNECTION_TEST_TIMEOUT_MS, &event),
                SALTS_OK);
    check_equal(event.kind, CHTTP_WEBSOCKET_EVENT_CLOSE);
    check_equal(flowie_connection_test_wait(&probe.closed), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.close_status, memory_order_relaxed),
                FLOWIE_CONNECTION_TEST_CLOSE_STATUS);
    check_equal(atomic_load_explicit(&probe.close_count, memory_order_relaxed), 1);

    check_equal(chttp_websocket_client_destroy(&client, FLOWIE_CONNECTION_TEST_TIMEOUT_MS),
                SALTS_OK);
    check_equal(flowie_server_stop(&server, FLOWIE_CONNECTION_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(flowie_server_destroy(&server), SALTS_OK);
  }
}
