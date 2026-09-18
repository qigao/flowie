#ifndef FLOWIE_CONNECTION_H
#define FLOWIE_CONNECTION_H

#include <http_server/http.h>
#include <cnet/cnet.h>
#include <salts/thread.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum flowie_transport {
  TF_NET_TRANSPORT_TCP = 1,
  TF_NET_TRANSPORT_TLS,
  TF_NET_TRANSPORT_UDP,
  TF_NET_TRANSPORT_KCP,
  TF_NET_TRANSPORT_WS,
  TF_NET_TRANSPORT_WSS
} flowie_transport;

typedef struct flowie_connection {
  uint32_t slot;
  uint32_t generation;
} flowie_connection;

typedef struct flowie_peer_info {
  cnet_stream_peer peer;
  char peer_certificate_sha256[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY];
} flowie_peer_info;

#define TF_NET_PEER_TEXT_CAPACITY 80u

int flowie_peer_format(const flowie_peer_info *peer, char *output, size_t capacity);

typedef int (*flowie_open_fn)(void *user, flowie_connection connection,
                              const flowie_peer_info *peer);
typedef int (*flowie_receive_fn)(void *user, flowie_connection connection, const void *data,
                                 size_t size);
typedef void (*flowie_close_fn)(void *user, flowie_connection connection, int status);
typedef void (*flowie_send_fn)(void *user, flowie_connection connection, size_t size);

typedef struct flowie_observer {
  flowie_open_fn on_open;
  flowie_receive_fn on_receive;
  flowie_close_fn on_close;
  flowie_send_fn on_send;
  void *user;
} flowie_observer;

typedef struct flowie_server_config {
  size_t size;
  flowie_transport transport;
  const char *host;
  uint16_t port;
  size_t backlog;
  const char *path;
  /** Optional exact WebSocket subprotocol token selected during WS/WSS upgrade. */
  const char *websocket_subprotocol;
  cnet_client_config stream;
  cnet_stream_socket_options stream_socket_options;
  cnet_listener_options listener_options;
  cnet_packet_endpoint_config packet;
  const cnet_tls_server_config *tls;
  size_t command_capacity;
  /** Aggregate copied payload bytes retained by the cross-thread command ring. */
  size_t command_bytes_capacity;
  size_t max_message_bytes;
  uint32_t poll_slice_ms;
  flowie_observer observer;
} flowie_server_config;

#define TF_NET_SERVER_CONFIG_INIT                                                                 \
  {sizeof(flowie_server_config), (flowie_transport)0, NULL, 0u, 0u, NULL, NULL, {0},              \
   CNET_STREAM_SOCKET_OPTIONS_INIT, CNET_LISTENER_OPTIONS_INIT,                                   \
   CNET_PACKET_ENDPOINT_CONFIG_INIT, NULL, 0u, 0u, 0u, 0u, {NULL, NULL, NULL, NULL, NULL}}

typedef struct flowie_server {
  void *impl;
} flowie_server;

int flowie_server_init(flowie_server *server, const flowie_server_config *config);
int flowie_server_start(flowie_server *server);
int flowie_server_port(const flowie_server *server, uint16_t *out_port);

/** Thread-safe copied admission; queue saturation returns SALTS_ENOBUFS. */
int flowie_server_send(flowie_server *server, flowie_connection connection, const void *data,
                       size_t size);
int flowie_server_close(flowie_server *server, flowie_connection connection, int status);

int flowie_server_stop(flowie_server *server, uint32_t timeout_ms);
int flowie_server_destroy(flowie_server *server);

#ifdef __cplusplus
}
#endif

#endif /* FLOWIE_CONNECTION_H */
