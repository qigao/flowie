#ifndef FLOWIE_MTLS_TEST_SERVER_H
#define FLOWIE_MTLS_TEST_SERVER_H

#include "platform.h"
#include "tls_test_support.h"
#include "cmeta_thread.h"

#include <gmssl/tls.h>
#include <gmssl/x509.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET flow_mtls_test_socket_t;
  #define FLOW_MTLS_TEST_INVALID_SOCKET INVALID_SOCKET
  #define flow_mtls_test_close_socket closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <signal.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
typedef int flow_mtls_test_socket_t;
  #define FLOW_MTLS_TEST_INVALID_SOCKET (-1)
  #define flow_mtls_test_close_socket close
#endif

#define FLOW_MTLS_TEST_REQUEST_CAPACITY 4096u

typedef struct flow_mtls_test_server_s {
  flow_mtls_test_socket_t listener;
  cmeta_thread_t thread;
  const uint8_t *response;
  size_t response_size;
  uint32_t response_delay_ms;
  unsigned short port;
  int started;
  int status;
  int require_peer_certificate;
  int peer_verified;
  int handshake_rejected;
  int abrupt_close;
  uint8_t request[FLOW_MTLS_TEST_REQUEST_CAPACITY];
  size_t request_size;
} flow_mtls_test_server_t;

/* Test-only GmSSL peer. Material is consumed synchronously by TLS_CTX; the
 * worker owns the context, connection and socket until it has joined. */
static int flow_mtls_test_server_context(TLS_CTX *ctx, int require_peer_certificate) {
  static const int ciphers[] = {TLS_cipher_aes_128_gcm_sha256,
      TLS_cipher_ecdhe_ecdsa_with_aes_128_gcm_sha256};
  static const int groups[] = {TLS_curve_secp256r1};
  static const int signatures[] = {TLS_sig_ecdsa_secp256r1_sha256};
  char cert_path[512] = {0}, key_path[512] = {0}, ca_path[512] = {0};
  int rc = -1;
  if (tls_ctx_init(ctx, TLS_protocol_tls13, TLS_server_mode) != 1 ||
      tls_ctx_set_protocol_range(ctx, TLS_protocol_tls12, TLS_protocol_tls13) != 1 ||
      tls_ctx_set_cipher_suites(ctx, ciphers, sizeof(ciphers) / sizeof(ciphers[0])) != 1 ||
      tls_ctx_set_supported_groups(ctx, groups, 1u) != 1 ||
      tls_ctx_set_signature_algorithms(ctx, signatures, 1u) != 1 ||
      tls_test_write_server_files(cert_path, sizeof(cert_path), key_path, sizeof(key_path)) != 0 ||
      tls_ctx_set_certificate_and_key(ctx, cert_path, key_path, "") != 1)
    goto done;
  if (require_peer_certificate &&
      (tls_test_write_ca_file(ca_path, sizeof(ca_path)) != 0 ||
       tls_ctx_set_ca_certificates(ctx, ca_path, 5) != 1 ||
       tls_ctx_enable_certificate_request(ctx, 1) != 1))
    goto done;
  rc = 0;
done:
  tls_test_remove_file(ca_path);
  tls_test_remove_file(key_path);
  tls_test_remove_file(cert_path);
  return rc;
}
static int flow_mtls_test_wait_readable(flow_mtls_test_socket_t socket_handle) {
  fd_set readfds;
  struct timeval timeout;
  FD_ZERO(&readfds);
  FD_SET(socket_handle, &readfds);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  return select((int)(socket_handle + 1), &readfds, NULL, NULL, &timeout) > 0;
}

static int flow_mtls_test_request_complete(const uint8_t *request, size_t size) {
  if (!request || size == 0u) return 0;
  if ((request[0] & 0xf0u) == 0x10u) {
    size_t multiplier = 1u;
    size_t remaining = 0u;
    size_t index = 1u;
    uint8_t encoded;
    do {
      if (index >= size || multiplier > 128u * 128u * 128u) return 0;
      encoded = request[index++];
      remaining += (size_t)(encoded & 0x7fu) * multiplier;
      multiplier *= 128u;
    } while ((encoded & 0x80u) != 0u);
    return size >= index + remaining;
  }
  if (size >= 4u) {
    const char *text = (const char *)request;
    const char *headers_end = strstr(text, "\r\n\r\n");
    if (headers_end) {
      const char *length_header = strstr(text, "Content-Length:");
      size_t header_size = (size_t)(headers_end + 4 - text);
      size_t content_size = length_header ? (size_t)strtoull(length_header + 15, NULL, 10) : 0u;
      return size >= header_size + content_size;
    }
  }
  return 0;
}

static void flow_mtls_test_server_main(void *arg) {
  flow_mtls_test_server_t *server = arg;
  flow_mtls_test_socket_t client = FLOW_MTLS_TEST_INVALID_SOCKET;
  TLS_CTX *ctx = calloc(1u, sizeof(*ctx));
  TLS_CONNECT *tls = calloc(1u, sizeof(*tls));
  const uint8_t *peer = NULL;
  size_t peer_size = 0u, sent = 0u;
  int verify_result = -1, initialized = 0, handshaken = 0;
  server->status = -1;
  if (!ctx || !tls || flow_mtls_test_server_context(ctx, server->require_peer_certificate) != 0 ||
      !flow_mtls_test_wait_readable(server->listener)) goto done;
  client = accept(server->listener, NULL, NULL);
  if (client == FLOW_MTLS_TEST_INVALID_SOCKET) goto done;
#ifdef _WIN32
  DWORD timeout = 5000u;
#else
  struct timeval timeout = {5, 0};
#endif
  if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) != 0 ||
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) != 0 ||
      tls_init(tls, ctx) != 1) goto done;
  initialized = 1;
  if (tls_set_socket(tls, client) != 1) goto done;
  if (tls_do_handshake(tls) != 1) {
    server->handshake_rejected = 1;
    goto done;
  }
  handshaken = 1;
  server->peer_verified = tls_get_peer_certificate(tls, &peer, &peer_size) == 1 &&
      peer && peer_size != 0u && tls_get_verify_result(tls, &verify_result) == 1 &&
      verify_result == X509_verify_ok;
  if (server->require_peer_certificate && !server->peer_verified) goto done;
  do {
    size_t received = 0u;
    if (tls_recv(tls, server->request + server->request_size,
                 sizeof(server->request) - 1u - server->request_size, &received) != 1 ||
        received == 0u) goto done;
    server->request_size += received;
    server->request[server->request_size] = '\0';
  } while (!flow_mtls_test_request_complete(server->request, server->request_size) &&
           server->request_size < sizeof(server->request) - 1u);
  if (!flow_mtls_test_request_complete(server->request, server->request_size)) goto done;
  if (server->response_delay_ms != 0u) cmeta_sleep_ms(server->response_delay_ms);
  while (sent < server->response_size) {
    size_t count = 0u;
    if (tls_send(tls, server->response + sent, server->response_size - sent, &count) != 1 ||
        count == 0u) goto done;
    sent += count;
  }
  server->status = 0;
done:
  if (handshaken && !server->abrupt_close) (void)tls_shutdown(tls);
  if (initialized) tls_cleanup(tls);
  if (client != FLOW_MTLS_TEST_INVALID_SOCKET) flow_mtls_test_close_socket(client);
  if (ctx) tls_ctx_cleanup(ctx);
  free(tls);
  free(ctx);
}
static int flow_mtls_test_server_start_ex(flow_mtls_test_server_t *server, const uint8_t *response,
                                          size_t response_size, uint32_t response_delay_ms,
                                          int require_peer_certificate, int abrupt_close) {
  struct sockaddr_storage address;
  int family = AF_INET6;
#ifdef _WIN32
  int address_size = (int)sizeof(address);
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
#else
  socklen_t address_size = (socklen_t)sizeof(address);
  signal(SIGPIPE, SIG_IGN);
#endif
  if (!server || (!response && response_size != 0u)) return -1;
  memset(server, 0, sizeof(*server));
  server->require_peer_certificate = require_peer_certificate ? 1 : 0;
  server->abrupt_close = abrupt_close ? 1 : 0;
  server->listener = socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (server->listener != FLOW_MTLS_TEST_INVALID_SOCKET) {
    int ipv6_only = 0;
    if (setsockopt(server->listener, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&ipv6_only,
                   sizeof(ipv6_only)) != 0) {
      flow_mtls_test_close_socket(server->listener);
      server->listener = FLOW_MTLS_TEST_INVALID_SOCKET;
    }
  }
  if (server->listener == FLOW_MTLS_TEST_INVALID_SOCKET) {
    family = AF_INET;
    server->listener = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (server->listener == FLOW_MTLS_TEST_INVALID_SOCKET) return -1;
  }
  memset(&address, 0, sizeof(address));
  if (family == AF_INET6) {
    struct sockaddr_in6 *address6 = (struct sockaddr_in6 *)&address;
    address6->sin6_family = AF_INET6;
    address6->sin6_addr = in6addr_any;
    address6->sin6_port = 0;
    address_size = (int)sizeof(*address6);
  } else {
    struct sockaddr_in *address4 = (struct sockaddr_in *)&address;
    address4->sin_family = AF_INET;
    address4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address4->sin_port = 0;
    address_size = (int)sizeof(*address4);
  }
  if (bind(server->listener, (const struct sockaddr *)&address, address_size) != 0 ||
      listen(server->listener, 1) != 0 ||
      getsockname(server->listener, (struct sockaddr *)&address, &address_size) != 0) {
    flow_mtls_test_close_socket(server->listener);
    server->listener = FLOW_MTLS_TEST_INVALID_SOCKET;
    return -1;
  }
  server->port = family == AF_INET6
                     ? ntohs(((const struct sockaddr_in6 *)&address)->sin6_port)
                     : ntohs(((const struct sockaddr_in *)&address)->sin_port);
  server->response = response;
  server->response_size = response_size;
  server->response_delay_ms = response_delay_ms;
  if (cmeta_thread_create(&server->thread, flow_mtls_test_server_main, server) != 0) {
    flow_mtls_test_close_socket(server->listener);
    server->listener = FLOW_MTLS_TEST_INVALID_SOCKET;
    return -1;
  }
  server->started = 1;
  return 0;
}

static int flow_mtls_test_server_start_delayed(flow_mtls_test_server_t *server,
                                               const uint8_t *response, size_t response_size,
                                               uint32_t response_delay_ms) {
  return flow_mtls_test_server_start_ex(server, response, response_size, response_delay_ms, 1, 0);
}

static int flow_tls_test_server_start(flow_mtls_test_server_t *server, const uint8_t *response,
                                      size_t response_size) {
  return flow_mtls_test_server_start_ex(server, response, response_size, 0u, 0, 0);
}

static int flow_tls_test_server_start_abrupt(flow_mtls_test_server_t *server) {
  return flow_mtls_test_server_start_ex(server, NULL, 0u, 0u, 0, 1);
}

static int flow_mtls_test_server_start(flow_mtls_test_server_t *server, const uint8_t *response,
                                       size_t response_size) {
  return flow_mtls_test_server_start_delayed(server, response, response_size, 0u);
}

static void flow_mtls_test_server_join(flow_mtls_test_server_t *server) {
  if (!server) return;
  if (server->started) (void)cmeta_thread_join(&server->thread);
  if (server->listener != FLOW_MTLS_TEST_INVALID_SOCKET)
    flow_mtls_test_close_socket(server->listener);
  server->listener = FLOW_MTLS_TEST_INVALID_SOCKET;
  server->started = 0;
}

#endif
