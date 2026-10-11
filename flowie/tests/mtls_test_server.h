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
  int abrupt_close;
  uint8_t request[FLOW_MTLS_TEST_REQUEST_CAPACITY];
  size_t request_size;
} flow_mtls_test_server_t;

static TLS_CTX *flow_mtls_test_server_context(int require_peer_certificate) {
  TLS_CTX *ctx = (TLS_CTX *)calloc(1, sizeof(*ctx));
  char cert_path[512] = {0}, key_path[512] = {0}, ca_path[512] = {0};
  const int ciphers[] = {TLS_cipher_aes_128_gcm_sha256,
                        TLS_cipher_ecdhe_ecdsa_with_aes_128_gcm_sha256,
                        TLS_cipher_ecdhe_rsa_with_aes_128_gcm_sha256};
  const int groups[] = {TLS_curve_secp256r1};
  const int signatures[] = {TLS_sig_ecdsa_secp256r1_sha256,
                            TLS_sig_rsa_pss_rsae_sha256, TLS_sig_rsa_pkcs1_sha256};
  int ready = 0;
  if (!ctx || tls_ctx_init(ctx, TLS_protocol_tls13, 0) != 1 ||
      tls_ctx_set_protocol_range(ctx, TLS_protocol_tls12, TLS_protocol_tls13) != 1 ||
      tls_ctx_set_cipher_suites(ctx, ciphers, sizeof(ciphers) / sizeof(ciphers[0])) != 1 ||
      tls_ctx_set_supported_groups(ctx, groups, sizeof(groups) / sizeof(groups[0])) != 1 ||
      tls_ctx_set_signature_algorithms(ctx, signatures,
                                        sizeof(signatures) / sizeof(signatures[0])) != 1 ||
      tls_test_write_server_files(cert_path, sizeof(cert_path), key_path, sizeof(key_path)) != 0 ||
      tls_ctx_set_certificate_and_key(ctx, cert_path, key_path, "") != 1) goto done;
  if (require_peer_certificate &&
      (tls_test_write_ca_file(ca_path, sizeof(ca_path)) != 0 ||
       tls_ctx_set_ca_certificates(ctx, ca_path, TLS_DEFAULT_VERIFY_DEPTH) != 1 ||
       tls_ctx_enable_certificate_request(ctx, 1) != 1 ||
       tls_ctx_enable_client_certificate_optional(ctx, 0) != 1)) goto done;
  ready = 1;
done:
  tls_test_remove_file(cert_path);
  tls_test_remove_file(key_path);
  tls_test_remove_file(ca_path);
  if (!ready && ctx) { tls_ctx_cleanup(ctx); free(ctx); ctx = NULL; }
  return ctx;
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
  flow_mtls_test_server_t *server = (flow_mtls_test_server_t *)arg;
  flow_mtls_test_socket_t client = FLOW_MTLS_TEST_INVALID_SOCKET;
  TLS_CTX *ctx = NULL;
  TLS_CONNECT *tls = NULL;
  int handshaken = 0, verify_result = -1;
  const uint8_t *peer = NULL;
  size_t peer_size = 0;
  uint8_t request[FLOW_MTLS_TEST_REQUEST_CAPACITY];
  size_t request_size = 0u;
  server->status = -1;
  ctx = flow_mtls_test_server_context(server->require_peer_certificate);
  if (!ctx || !flow_mtls_test_wait_readable(server->listener)) goto done;
  client = accept(server->listener, NULL, NULL);
  if (client == FLOW_MTLS_TEST_INVALID_SOCKET) goto done;
  /* Bound every blocking handshake/read/write so failed peers cannot hang CTest. */
#ifdef _WIN32
  { DWORD timeout = 5000;
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) != 0 ||
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) != 0)
      goto done;
  }
#else
  { struct timeval timeout = {5, 0};
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) goto done;
  }
#endif
  tls = (TLS_CONNECT *)calloc(1, sizeof(*tls));
  if (!tls || tls_init(tls, ctx) != 1 || tls_set_socket(tls, client) != 1 ||
      tls_do_handshake(tls) != 1) goto done;
  handshaken = 1;
  server->peer_verified = tls_get_peer_certificate(tls, &peer, &peer_size) == 1 &&
      peer != NULL && peer_size != 0 && tls_get_verify_result(tls, &verify_result) == 1 &&
      verify_result == X509_verify_ok;
  if (server->require_peer_certificate && !server->peer_verified) goto done;
  do {
    size_t received = 0;
    if (tls_recv(tls, request + request_size, sizeof(request) - 1u - request_size, &received) != 1 ||
        received == 0) goto done;
    request_size += (size_t)received;
    request[request_size] = '\0';
  } while (!flow_mtls_test_request_complete(request, request_size) &&
           request_size < sizeof(request) - 1u);
  if (!flow_mtls_test_request_complete(request, request_size)) goto done;
  memcpy(server->request, request, request_size + 1u);
  server->request_size = request_size;
  if (server->response_delay_ms != 0u) cmeta_sleep_ms(server->response_delay_ms);
  for (size_t offset = 0; offset < server->response_size;) {
    size_t sent = 0;
    if (tls_send(tls, server->response + offset, server->response_size - offset, &sent) != 1 ||
        sent == 0) goto done;
    offset += sent;
  }
  server->status = 0;
done:
  if (handshaken && !server->abrupt_close) (void)tls_shutdown(tls);
  if (tls) { tls_cleanup(tls); free(tls); }
  if (client != FLOW_MTLS_TEST_INVALID_SOCKET) flow_mtls_test_close_socket(client);
  if (ctx) { tls_ctx_cleanup(ctx); free(ctx); }
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
