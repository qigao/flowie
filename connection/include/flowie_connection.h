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

#define FLOWIE_NETWORK_WORKERS_MAX 64u

typedef enum flowie_owner_policy {
  TF_NET_OWNER_ROUND_ROBIN = 0,
  TF_NET_OWNER_LEAST_CONNECTIONS = 1
} flowie_owner_policy;

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
  /** Aggregate queued payload bytes retained by the cross-thread command mailbox. */
  size_t command_bytes_capacity;
  size_t max_message_bytes;
  uint32_t poll_slice_ms;
  flowie_observer observer;
  /**
   * TCP/TLS progress owners. 0/1 preserves the single-owner path; maximum 64.
   * Connection and mailbox-byte capacities are aggregate and partitioned across
   * owners; each partition must hold max_message_bytes. Other CNet capacities
   * and command_capacity apply per owner. Callbacks for different connections
   * may overlap; one connection keeps its owner and ordered callbacks until close.
   * UDP/KCP reject values greater than one with SALTS_ENOTSUP.
   * WS/WSS delegate network_workers and placement to CHttp's fixed-Owner
   * runtime; Flowie does not create another WebSocket scheduler. Explicit
   * per-Owner CPU affinity remains unsupported for WebSocket.
   */
  uint32_t network_workers;
  /** TCP/TLS and CHttp WS/WSS accept placement; includes handoff pressure. */
  flowie_owner_policy network_policy;
  /** 0 preserves OS scheduling; otherwise exactly one CPU per effective owner. */
  uint32_t network_cpu_count;
  /** Copied CPU IDs. Windows: group * 64 + processor; Linux/Android: logical ID. */
  uint32_t network_cpus[FLOWIE_NETWORK_WORKERS_MAX];
} flowie_server_config;

#define TF_NET_SERVER_CONFIG_INIT                                                                 \
  {sizeof(flowie_server_config), (flowie_transport)0, NULL, 0u, 0u, NULL, NULL, {0},              \
   CNET_STREAM_SOCKET_OPTIONS_INIT, CNET_LISTENER_OPTIONS_INIT,                                   \
   CNET_PACKET_ENDPOINT_CONFIG_INIT, NULL, 0u, 0u, 0u, 0u, {NULL, NULL, NULL, NULL, NULL}}

typedef struct flowie_server {
  void *impl;
} flowie_server;

int flowie_server_init(flowie_server *server, const flowie_server_config *config);
/** Explicit CPU binding errors fail start and join started workers; recreate to retry. */
int flowie_server_start(flowie_server *server);
int flowie_server_port(const flowie_server *server, uint16_t *out_port);

/** Thread-safe copied admission; queue saturation returns SALTS_ENOBUFS. */
int flowie_server_send(flowie_server *server, flowie_connection connection, const void *data,
                       size_t size);

/**
 * Thread-safe scatter/gather admission for TCP/TLS streams. Before SALTS_OK,
 * bytes are retained or copied into bounded small-packet storage. The caller
 * may then release its slice references; any still-shared backing must remain
 * immutable. Failed admission leaves the caller's references unchanged.
 */
int flowie_server_send_slicev(flowie_server *server, flowie_connection connection,
                              const mem_slice_t *segments, size_t segment_count);
int flowie_server_close(flowie_server *server, flowie_connection connection, int status);

int flowie_server_stop(flowie_server *server, uint32_t timeout_ms);
int flowie_server_destroy(flowie_server *server);

#ifdef __cplusplus
}
#endif

#endif /* FLOWIE_CONNECTION_H */
