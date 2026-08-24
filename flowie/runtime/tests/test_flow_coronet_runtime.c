#include "flow_coronet_runtime.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

#define FLOW_CORONET_RUNTIME_TEST_TIMEOUT_MS 3000u

typedef struct flow_coronet_runtime_case_s {
  coro_context_t *context;
  coro_socket_t *server;
  tf_coronet_transport_t transport;
  const char *host;
  const char *path;
  int port;
  int handler_done;
  int handler_rc;
  int client_done;
  int client_rc;
  char received[32];
  size_t received_size;
} flow_coronet_runtime_case_t;

static unsigned short flow_coronet_runtime_udp_port(void) {
  struct sockaddr_in address;
#ifdef _WIN32
  SOCKET socket_handle = INVALID_SOCKET;
  int address_size = (int)sizeof(address);
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0u;
#else
  int socket_handle = -1;
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  unsigned short port = 0u;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
  if (socket_handle == INVALID_SOCKET) {
    WSACleanup();
    return 0u;
  }
#else
  if (socket_handle < 0) return 0u;
#endif
  if (bind(socket_handle, (struct sockaddr *)&address, sizeof(address)) == 0 &&
      getsockname(socket_handle, (struct sockaddr *)&address, &address_size) == 0) {
    port = ntohs(address.sin_port);
  }
#ifdef _WIN32
  closesocket(socket_handle);
  WSACleanup();
#else
  close(socket_handle);
#endif
  return port;
}

static void flow_coronet_runtime_handler(coro_socket_t *client, void *arg) {
  flow_coronet_runtime_case_t *state = (flow_coronet_runtime_case_t *)arg;
  char *data = NULL;
  size_t size = 0u;

  state->handler_rc = coro_socket_recv(client, &data, &size);
  if (state->handler_rc == TURBO_OK && data && size < sizeof(state->received)) {
    memcpy(state->received, data, size);
    state->received[size] = '\0';
    state->received_size = size;
  }
  if (data) coro_socket_free_recv(data);
  state->handler_done = 1;
}

static void flow_coronet_runtime_client(coro_t *co, void *arg) {
  static const char payload[] = "flowie-runtime";
  flow_coronet_runtime_case_t *state = (flow_coronet_runtime_case_t *)arg;
  coro_socket_t *client;
  uint64_t deadline;

  (void)co;
  client = tf_coronet_create_client_socket(state->context, state->transport);
  if (!client) {
    state->client_rc = TURBO_ENOMEM;
    state->client_done = 1;
    return;
  }
  (void)coro_socket_set_timeout(client, FLOW_CORONET_RUNTIME_TEST_TIMEOUT_MS);
  state->client_rc =
      tf_coronet_connect_socket(client, state->transport, state->host, state->port, state->path);
  if (state->client_rc == TURBO_OK) {
    state->client_rc =
        tf_coronet_send_socket(client, state->transport, payload, sizeof(payload) - 1u);
  }
  deadline = turbo_monotonic_ms() + FLOW_CORONET_RUNTIME_TEST_TIMEOUT_MS;
  while (state->client_rc == TURBO_OK && !state->handler_done && turbo_monotonic_ms() < deadline)
    coro_sleep(state->context, 1u);
  if (state->client_rc == TURBO_OK && !state->handler_done) state->client_rc = TURBO_ETIMEDOUT;
  coro_socket_destroy(client);
  state->client_done = 1;
}

static int flow_coronet_runtime_done(void *arg) {
  const flow_coronet_runtime_case_t *state = (const flow_coronet_runtime_case_t *)arg;
  return state->handler_done && state->client_done;
}

static int flow_coronet_runtime_server_stopped(void *arg) {
  const flow_coronet_runtime_case_t *state = (const flow_coronet_runtime_case_t *)arg;
  return coro_socket_server_is_stopped(state->server);
}

static int flow_coronet_runtime_run_until(coro_context_t *context, int (*predicate)(void *),
                                          void *arg, uint64_t timeout_ms) {
  uint64_t deadline = turbo_monotonic_ms() + timeout_ms;
  while (!predicate(arg) && turbo_monotonic_ms() < deadline)
    coro_context_run(context, TURBO_RUN_ONCE);
  return predicate(arg) ? TURBO_OK : TURBO_ETIMEDOUT;
}

static void flow_coronet_runtime_cleanup(flow_coronet_runtime_case_t *state) {
  int drain = 500;
  if (!state || !state->context) return;
  if (state->server) {
    (void)coro_socket_server_stop(state->server);
    (void)flow_coronet_runtime_run_until(state->context, flow_coronet_runtime_server_stopped, state,
                                         FLOW_CORONET_RUNTIME_TEST_TIMEOUT_MS);
    coro_socket_destroy(state->server);
  }
  coro_context_stop(state->context);
  while (coro_context_alive(state->context) && drain-- > 0)
    coro_context_run(state->context, TURBO_RUN_NOWAIT);
  coro_context_destroy(state->context);
}

static void flow_coronet_runtime_round_trip(flow_coronet_runtime_case_t *state) {
  static const char payload[] = "flowie-runtime";

  state->context = coro_context_create(NULL);
  check_not_null(state->context);
  state->server = tf_coronet_create_server_socket(state->context, state->transport);
  check_not_null(state->server);
  check_equal(tf_coronet_listen_socket(state->server, state->transport, state->host, state->port,
                                       state->path, flow_coronet_runtime_handler, state),
              TURBO_OK);
  check_equal(coro_context_spawn(state->context, flow_coronet_runtime_client, state), TURBO_OK);
  check_equal(flow_coronet_runtime_run_until(state->context, flow_coronet_runtime_done, state,
                                             FLOW_CORONET_RUNTIME_TEST_TIMEOUT_MS),
              TURBO_OK);
  check_equal(state->client_rc, TURBO_OK);
  check_equal(state->handler_rc, TURBO_OK);
  check_equal(state->received_size, sizeof(payload) - 1u);
  check_equal(state->received, payload);
  flow_coronet_runtime_cleanup(state);
}

spec("Flowie CoroNet runtime transports") {
  it("routes a UDP datagram through the Flowie runtime adapter") {
    flow_coronet_runtime_case_t state = {0};
    state.transport = TF_CORONET_TRANSPORT_UDP;
    state.host = "127.0.0.1";
    state.port = (int)flow_coronet_runtime_udp_port();
    state.handler_rc = TURBO_EBUSY;
    state.client_rc = TURBO_EBUSY;
    check_greater(state.port, 0);
    flow_coronet_runtime_round_trip(&state);
  }

  it("routes a Unix pipe payload through the Flowie runtime adapter") {
    flow_coronet_runtime_case_t state = {0};
    char path[160];
#ifdef _WIN32
    check_true(snprintf(path, sizeof(path), "pipe://flowie_runtime_%lu_%llu",
                        (unsigned long)GetCurrentProcessId(),
                        (unsigned long long)turbo_monotonic_ms()) > 0);
#else
    check_true(snprintf(path, sizeof(path), "/tmp/flowie_runtime_%llu_%llu.sock",
                        (unsigned long long)getpid(),
                        (unsigned long long)turbo_monotonic_ms()) > 0);
    (void)unlink(path);
#endif
    state.transport = TF_CORONET_TRANSPORT_PIPE;
    state.path = path;
    state.handler_rc = TURBO_EBUSY;
    state.client_rc = TURBO_EBUSY;
    flow_coronet_runtime_round_trip(&state);
#ifndef _WIN32
    (void)unlink(path);
#endif
  }

  it("applies UDP options only to UDP sockets") {
    tf_coronet_udp_options_t options = {0};
    coro_context_t *context = coro_context_create(NULL);
    coro_socket_t *udp;
    int port = (int)flow_coronet_runtime_udp_port();

    check_not_null(context);
    check_greater(port, 0);
    udp = tf_coronet_create_client_socket(context, TF_CORONET_TRANSPORT_UDP);
    check_not_null(udp);
    check_equal(tf_coronet_connect_socket(udp, TF_CORONET_TRANSPORT_UDP, "127.0.0.1", port, NULL),
                TURBO_OK);
    options.option_flags = TF_CORONET_UDP_OPTION_MULTICAST_LOOP |
                           TF_CORONET_UDP_OPTION_MULTICAST_TTL | TF_CORONET_UDP_OPTION_BROADCAST;
    options.multicast_loop = 1;
    options.multicast_ttl = 2u;
    options.broadcast = 1;
    check_equal(tf_coronet_udp_options_validate(TF_CORONET_TRANSPORT_UDP, &options, 1), TURBO_OK);
    check_equal(tf_coronet_apply_udp_options(udp, TF_CORONET_TRANSPORT_UDP, &options), TURBO_OK);
    check_equal(tf_coronet_udp_options_validate(TF_CORONET_TRANSPORT_TCP, &options, 1),
                TURBO_EINVAL);
#if defined(__linux__) || defined(__ANDROID__)
    check_equal(coro_socket_get_udp_backend(udp), TURBO_UDP_BACKEND_EPOLL);
#endif
    coro_socket_destroy(udp);
    coro_context_destroy(context);
  }
}
