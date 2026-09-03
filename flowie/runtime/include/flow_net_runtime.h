#ifndef FLOW_NET_RUNTIME_H
#define FLOW_NET_RUNTIME_H

#include <chttp/chttp.h>
#include <cnet/cnet.h>
#include <salts/thread.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tf_net_transport {
  TF_NET_TRANSPORT_TCP = 1,
  TF_NET_TRANSPORT_TLS,
  TF_NET_TRANSPORT_UDP,
  TF_NET_TRANSPORT_KCP,
  TF_NET_TRANSPORT_WS,
  TF_NET_TRANSPORT_WSS
} tf_net_transport;

typedef struct tf_net_connection {
  uint32_t slot;
  uint32_t generation;
} tf_net_connection;

typedef struct tf_net_peer_info {
  cnet_stream_peer peer;
  char peer_certificate_sha256[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY];
} tf_net_peer_info;

#define TF_NET_PEER_TEXT_CAPACITY 80u

int tf_net_peer_format(const tf_net_peer_info *peer, char *output, size_t capacity);

typedef int (*tf_net_open_fn)(void *user, tf_net_connection connection,
                              const tf_net_peer_info *peer);
typedef int (*tf_net_receive_fn)(void *user, tf_net_connection connection, const void *data,
                                 size_t size);
typedef void (*tf_net_close_fn)(void *user, tf_net_connection connection, int status);
typedef void (*tf_net_send_fn)(void *user, tf_net_connection connection, size_t size);

typedef struct tf_net_observer {
  tf_net_open_fn on_open;
  tf_net_receive_fn on_receive;
  tf_net_close_fn on_close;
  tf_net_send_fn on_send;
  void *user;
} tf_net_observer;

typedef struct tf_net_server_config {
  size_t size;
  tf_net_transport transport;
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
  tf_net_observer observer;
} tf_net_server_config;

#define TF_NET_SERVER_CONFIG_INIT                                                                 \
  {sizeof(tf_net_server_config), (tf_net_transport)0, NULL, 0u, 0u, NULL, NULL, {0},              \
   CNET_STREAM_SOCKET_OPTIONS_INIT, CNET_LISTENER_OPTIONS_INIT,                                   \
   CNET_PACKET_ENDPOINT_CONFIG_INIT, NULL, 0u, 0u, 0u, 0u, {NULL, NULL, NULL, NULL, NULL}}

typedef struct tf_net_server {
  void *impl;
} tf_net_server;

int tf_net_server_init(tf_net_server *server, const tf_net_server_config *config);
int tf_net_server_start(tf_net_server *server);
int tf_net_server_port(const tf_net_server *server, uint16_t *out_port);

/** Thread-safe copied admission; queue saturation returns SALTS_ENOBUFS. */
int tf_net_server_send(tf_net_server *server, tf_net_connection connection, const void *data,
                       size_t size);
int tf_net_server_close(tf_net_server *server, tf_net_connection connection, int status);

int tf_net_server_stop(tf_net_server *server, uint32_t timeout_ms);
int tf_net_server_destroy(tf_net_server *server);

#ifdef __cplusplus
}
#endif

#endif /* FLOW_NET_RUNTIME_H */
