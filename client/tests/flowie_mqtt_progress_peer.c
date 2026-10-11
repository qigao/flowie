#include "flowie_mqtt_progress_support.h"
#include "flowie_connection.h"
#include "flowie_mqtt_protocol.h"
#include "tls_test_support.h"
#include <stdatomic.h>

typedef struct progress_peer {
  flowie_server server;
  flowie_connection connection;
  unsigned char input[1024];
  size_t used;
  atomic_int connected;
  atomic_int error;
  atomic_int opens;
  atomic_int closes;
} progress_peer;

static int progress_peer_open(void *user, flowie_connection connection,
                              const flowie_peer_info *info) {
  progress_peer *peer = user;
  (void)connection;
  if (!info || atomic_fetch_add(&peer->opens, 1) != 0) return SALTS_EPROTO;
  return SALTS_OK;
}

static void progress_peer_close(void *user, flowie_connection connection, int status) {
  progress_peer *peer = user;
  (void)connection;
  atomic_fetch_add(&peer->closes, 1);
  if (status != SALTS_OK && status != SALTS_EOF && status != SALTS_ECONNRESET)
    atomic_store(&peer->error, status);
}

static int progress_peer_receive(void *user, flowie_connection connection,
                                 const void *data, size_t size) {
  progress_peer *peer = user;
  static const unsigned char connack[] = {0x20, 0x03, 0, 0, 0};
  if (size > sizeof(peer->input) - peer->used) return SALTS_EMSGSIZE;
  memcpy(peer->input + peer->used, data, size);
  peer->used += size;
  size_t remaining = 0, multiplier = 1, index = 1;
  for (; index < peer->used && index <= 4; ++index) {
    const unsigned char digit = peer->input[index];
    remaining += (digit & 127u) * multiplier;
    if (!(digit & 128u)) break;
    multiplier *= 128;
  }
  if (index == peer->used) return SALTS_OK;
  if (index > 4 || peer->input[0] != 0x10 ||
      atomic_load_explicit(&peer->connected, memory_order_acquire)) {
    atomic_store(&peer->error, SALTS_EPROTO);
    return SALTS_EPROTO;
  }
  if (index + 1 + remaining > peer->used) return SALTS_OK;
  if (index + 1 + remaining != peer->used) return SALTS_EPROTO;
  const int rc = flowie_server_send(&peer->server, connection, connack, sizeof(connack));
  if (rc == SALTS_OK) {
    peer->connection = connection;
    atomic_store_explicit(&peer->connected, 1, memory_order_release);
  } else atomic_store(&peer->error, rc);
  return rc;
}

static int progress_peer_run(int mode) {
  progress_peer peer = {0};
  atomic_init(&peer.connected, 0);
  atomic_init(&peer.error, SALTS_OK);
  atomic_init(&peer.opens, 0);
  atomic_init(&peer.closes, 0);
  const flowie_transport transports[] = {TF_NET_TRANSPORT_TCP, TF_NET_TRANSPORT_TLS,
                                         TF_NET_TRANSPORT_WS, TF_NET_TRANSPORT_WSS};
  flowie_server_config config = TF_NET_SERVER_CONFIG_INIT;
  cnet_tls_server_config tls = {.size = sizeof(tls)};
  char cert[512] = {0}, key[512] = {0};
  progress_control reply = {0}, command = {0};
  uint16_t port = 0;
  int rc = SALTS_OK;
  if (mode == 1 || mode == 3) {
    if (tls_test_write_server_files(cert, sizeof(cert), key, sizeof(key)) != 0)
      return SALTS_EIO;
    tls.cert_file = cert;
    tls.key_file = key;
    tls.key_password = "";
    config.tls = &tls;
  }
  config.transport = transports[mode];
  config.host = "127.0.0.1";
  config.path = "/mqtt";
  config.websocket_subprotocol = mode >= 2 ? "mqtt" : NULL;
  config.backlog = 4;
  config.stream = (cnet_client_config){
      .backend = NATIVE_IO_BACKEND_EPOLL, .connection_capacity = 4,
      .command_capacity = 16, .request_capacity = 8, .completion_batch_capacity = 8,
      .event_capacity = 16, .max_send_bytes = 8192, .receive_buffer_bytes = 8192,
      .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
      .tls_handshake_timeout_ms = PROGRESS_TIMEOUT_MS,
      .connect_timeout_ms = PROGRESS_TIMEOUT_MS, .read_timeout_ms = PROGRESS_TIMEOUT_MS,
      .write_timeout_ms = PROGRESS_TIMEOUT_MS};
  config.command_capacity = 64;
  config.command_bytes_capacity = 32768;
  config.max_message_bytes = 4096;
  config.poll_slice_ms = 1;
  config.observer.on_open = progress_peer_open;
  config.observer.on_receive = progress_peer_receive;
  config.observer.on_close = progress_peer_close;
  config.observer.user = &peer;
  rc = flowie_server_init(&peer.server, &config);
  if (rc == SALTS_OK) rc = flowie_server_start(&peer.server);
  if (rc == SALTS_OK) rc = flowie_server_port(&peer.server, &port);
  reply.value = port;
  reply.status = rc;
  if (progress_write(STDIN_FILENO, &reply) != SALTS_OK) { rc = SALTS_EIO; goto done; }
  if (rc != SALTS_OK || progress_read(STDIN_FILENO, &command) != SALTS_OK) goto done;
  if (command.value != PROGRESS_SAMPLES || !atomic_load_explicit(&peer.connected, memory_order_acquire)) {
    rc = SALTS_EPROTO;
    goto done;
  }
  const uint64_t start = cmeta_hrtime() + UINT64_C(50000000);
  for (size_t i = 0; i < PROGRESS_SAMPLES; ++i) {
    unsigned char payload[PROGRESS_PAYLOAD_BYTES], wire[256];
    memset(payload, 0x5a, sizeof(payload));
    const uint64_t seq = i, planned = start + i * PROGRESS_INTERVAL_NS;
    memcpy(payload, &seq, sizeof(seq));
    memcpy(payload + 8, &planned, sizeof(planned));
    flowie_mqtt_publish_packet_t packet = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
    packet.version = FLOWIE_MQTT_VERSION_5;
    packet.topic = (flowie_mqtt_span_t){(const uint8_t *)PROGRESS_TOPIC, sizeof(PROGRESS_TOPIC) - 1};
    packet.payload = (flowie_mqtt_span_t){payload, sizeof(payload)};
    size_t written = 0;
    rc = flowie_mqtt_publish_packet_encode(&packet, wire, sizeof(wire), &written);
    if (rc != SALTS_OK || (rc = progress_sleep_until(planned)) != SALTS_OK) break;
    const uint64_t attempted = cmeta_hrtime();
    memcpy(wire + written - PROGRESS_PAYLOAD_BYTES + 16, &attempted, sizeof(attempted));
    do {
      rc = flowie_server_send(&peer.server, peer.connection, wire, written);
      if (rc != SALTS_ENOBUFS) break;
      ++reply.retries;
      if (cmeta_hrtime() - attempted >= UINT64_C(5000000000)) { rc = SALTS_ETIMEDOUT; break; }
      cmeta_sleep_ms(1);
    } while (1);
    if (rc != SALTS_OK) break;
  }
  if (rc == SALTS_OK) rc = atomic_load(&peer.error);
  reply.status = rc;
  if (progress_write(STDIN_FILENO, &reply) != SALTS_OK) rc = SALTS_EIO;
  /* Parent acknowledges validated receipt before the peer closes the socket. */
  if (progress_read(STDIN_FILENO, &command) != SALTS_OK && rc == SALTS_OK) rc = SALTS_EIO;
done:
  if (peer.server.impl) {
    const int stopped = flowie_server_stop(&peer.server, 5000);
    const int destroyed = stopped == SALTS_OK ? flowie_server_destroy(&peer.server) : stopped;
    if (rc == SALTS_OK) rc = destroyed;
    if (rc == SALTS_OK && atomic_load(&peer.connected) && atomic_load(&peer.closes) != 1)
      rc = SALTS_EPROTO;
  }
  tls_test_remove_file(key);
  tls_test_remove_file(cert);
  return rc;
}

int main(int argc, char **argv) {
  if (argc != 2 || argv[1][0] < '0' || argv[1][0] > '3' || argv[1][1]) return 2;
  /* IPC is bounded even if the controller dies during a measurement. */
  struct timeval timeout = {30, 0};
  if (setsockopt(STDIN_FILENO, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
      setsockopt(STDIN_FILENO, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) return 2;
  return progress_peer_run(argv[1][0] - '0') == SALTS_OK ? 0 : 1;
}
