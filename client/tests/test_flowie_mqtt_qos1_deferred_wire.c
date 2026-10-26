/* End-to-end native Flowie Worker QoS1 settlement protocol acceptance.
 * An actual loopback Flowie Broker sends a PUBLISH and DUP while the
 * Client callback only admits a borrowed message and copied token.
 * No PUBACK is authorized until an external "consumer Owner" explicitly
 * calls settle_inbound_qos1(). PING must progress before settlement.
 * The test does not pretend to implement a durable business DB.
 */
#include "flowie_mqtt_client.h"
#include "flowie_connection.h"
#include "flowie_mqtt_protocol.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/error_codes.h>
#include <cnet/cnet.h>
#include <stdatomic.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TEST_TIMEOUT_MS = 7000u, TEST_FRAME_MAX = 1024u, TEST_NEED_MORE = 1 };
typedef struct broker_s {
  flowie_server *server;
  unsigned char framing[TEST_FRAME_MAX];
  size_t pending;
  atomic_uint pubacks;
  atomic_uint pingreqs;
  atomic_uint connects;
  atomic_int error;
} broker;
typedef struct probe_s {
  flowie_mqtt_client_inbound_token_t token;
  atomic_uint deliveries;
  atomic_uint legacy_deliveries;
  atomic_int connected;
  atomic_int pinged;
  atomic_int first_settlement;
  atomic_int second_settlement;
  atomic_int stale_settlement;
  atomic_int callback_self_settlement;
  atomic_uint errors;
} probe;

static native_io_backend_kind test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int frame_size(const unsigned char *p, size_t len, size_t *out) {
  size_t value = 0u, factor = 1u, i;
  if (!p || !out) return SALTS_EINVAL;
  for (i = 1u; i < len && i <= 4u; ++i) {
    const unsigned char c = p[i];
    value += (size_t)(c & 0x7fu) * factor;
    if (!(c & 0x80u)) {
      *out = value + i + 1u;
      return *out <= len ? SALTS_OK : TEST_NEED_MORE;
    }
    factor *= 128u;
  }
  return TEST_NEED_MORE;
}
static int server_open(void *user, flowie_connection c, const flowie_peer_info *peer) {
  (void)user; (void)c;
  return peer ? SALTS_OK : SALTS_EINVAL;
}
static int server_receive(void *user, flowie_connection c,
                          const void *bytes, size_t n) {
  static const unsigned char connack[] = {0x20u,0x03u,0u,0u,0u};
  static const unsigned char qos1[] = {
      0x32u,0x0eu,0u,4u,'t','e','s','t',0u,7u,0u,'h','e','l','l','o'};
  static const unsigned char qos1_dup[] = {
      0x3au,0x0eu,0u,4u,'t','e','s','t',0u,7u,0u,'h','e','l','l','o'};
  static const unsigned char pingresp[] = {0xd0u,0u};
  broker *b = (broker *)user;
  if (!b || !bytes || n > sizeof(b->framing) - b->pending) return SALTS_EMSGSIZE;
  memcpy(b->framing + b->pending, bytes, n);
  b->pending += n;
  while (b->pending) {
    size_t size = 0u;
    int rc = frame_size(b->framing, b->pending, &size);
    if (rc == TEST_NEED_MORE) return SALTS_OK;
    if (rc != SALTS_OK || !size || size > b->pending) return SALTS_EPROTO;
    switch (b->framing[0] >> 4u) {
    case FLOWIE_MQTT_PACKET_CONNECT:
      atomic_fetch_add_explicit(&b->connects, 1u, memory_order_release);
      rc = flowie_server_send(b->server, c, connack, sizeof(connack));
      if (rc == SALTS_OK)
        rc = flowie_server_send(b->server, c, qos1, sizeof(qos1));
      if (rc == SALTS_OK)
        rc = flowie_server_send(b->server, c, qos1_dup, sizeof(qos1_dup));
      break;
    case FLOWIE_MQTT_PACKET_PINGREQ:
      atomic_fetch_add_explicit(&b->pingreqs, 1u, memory_order_release);
      rc = flowie_server_send(b->server, c, pingresp, sizeof(pingresp));
      break;
    case FLOWIE_MQTT_PACKET_PUBACK:
      if (size != 6u && size != 4u) return SALTS_EPROTO;
      if (b->framing[2] != 0u || b->framing[3] != 7u) return SALTS_EPROTO;
      atomic_fetch_add_explicit(&b->pubacks, 1u, memory_order_release);
      break;
    case FLOWIE_MQTT_PACKET_DISCONNECT:
      break;
    default:
      atomic_store_explicit(&b->error, SALTS_EPROTO, memory_order_release);
      return SALTS_EPROTO;
    }
    if (rc != SALTS_OK) return rc;
    memmove(b->framing, b->framing + size, b->pending - size);
    b->pending -= size;
  }
  return SALTS_OK;
}
static void server_close(void *user, flowie_connection c, int status) {
  (void)user;(void)c;(void)status;
}
static int legacy_receive(flowie_mqtt_client_t *c,
                          const flowie_mqtt_publish_view_t *m, void *u) {
  probe *p = (probe *)u;
  (void)c;(void)m;
  atomic_fetch_add_explicit(&p->legacy_deliveries,1u,memory_order_release);
  return SALTS_OK;
}
static void settled_first(flowie_mqtt_client_t *c,int status,
                          const flowie_mqtt_control_packet_view_t *ack,void *u);

static int deferred_receive(flowie_mqtt_client_t *c,
                            const flowie_mqtt_publish_view_t *m,
                            const flowie_mqtt_client_inbound_token_t *token,
                            void *u) {
  probe *p = (probe *)u;
  (void)c;
  if (!m || !token || m->qos != 1u || token->packet_id != 7u ||
      m->payload.size != 5u || memcmp(m->payload.data,"hello",5u))
    return SALTS_EPROTO;
  p->token = *token; /* copy; message view ceases on callback return */
  /* The borrowed callback cannot wait for itself or fabricate a PUBACK.
   * A true external Consumer Owner will settle only after DB COMMIT. */
  atomic_store_explicit(&p->callback_self_settlement,
      flowie_mqtt_client_settle_inbound_qos1(
          c,token,settled_first,p),memory_order_release);
  atomic_fetch_add_explicit(&p->deliveries,1u,memory_order_release);
  return SALTS_OK; /* NOT a PUBACK */
}
static void connected(flowie_mqtt_client_t *c,int status,
                      const flowie_mqtt_control_packet_view_t *ack,void *u) {
  probe *p = (probe *)u;(void)c;
  atomic_store_explicit(&p->connected, status == SALTS_OK && ack &&
             ack->type == FLOWIE_MQTT_PACKET_CONNACK && ack->reason_code == 0u
             ? SALTS_OK : SALTS_EPROTO, memory_order_release);
}
static void pinged(flowie_mqtt_client_t *c,int status,
                   const flowie_mqtt_control_packet_view_t *ack,void *u) {
  (void)c;(void)ack;
  atomic_store_explicit(&((probe*)u)->pinged,status == SALTS_OK ? SALTS_OK : status,
                        memory_order_release);
}
static void settled_first(flowie_mqtt_client_t *c,int status,
                          const flowie_mqtt_control_packet_view_t *ack,void *u) {
  (void)c;(void)ack;
  atomic_store_explicit(&((probe*)u)->first_settlement,
                        status == SALTS_OK ? SALTS_OK : status,
                        memory_order_release);
}
static void settled_again(flowie_mqtt_client_t *c,int status,
                          const flowie_mqtt_control_packet_view_t *ack,void *u) {
  (void)c;(void)ack;
  atomic_store_explicit(&((probe*)u)->second_settlement,status,
                        memory_order_release);
}
static void settled_stale(flowie_mqtt_client_t *c,int status,
                          const flowie_mqtt_control_packet_view_t *ack,void *u) {
  (void)c;(void)ack;
  atomic_store_explicit(&((probe*)u)->stale_settlement,status,
                        memory_order_release);
}
static void error_event(flowie_mqtt_client_t *c,int status,void *u) {
  (void)c;(void)status;
  atomic_fetch_add_explicit(&((probe*)u)->errors,1u,memory_order_release);
}
static int wait_uint(atomic_uint *p,unsigned expected) {
  const uint64_t until = cmeta_monotonic_ms() + TEST_TIMEOUT_MS;
  while (atomic_load_explicit(p,memory_order_acquire)<expected &&
         cmeta_monotonic_ms()<until) cmeta_sleep_ms(1u);
  return atomic_load_explicit(p,memory_order_acquire)>=expected;
}
static int wait_status(atomic_int *p,int expected) {
  const uint64_t until = cmeta_monotonic_ms() + TEST_TIMEOUT_MS;
  while (atomic_load_explicit(p,memory_order_acquire)==SALTS_EBUSY &&
         cmeta_monotonic_ms()<until) cmeta_sleep_ms(1u);
  return atomic_load_explicit(p,memory_order_acquire)==expected;
}

int main(void) {
  static const unsigned char client_id[]="deferred-qos1-test";
  static const unsigned char filter[]="test";
  flowie_server server={0};
  flowie_server_config conf = {
      .size = sizeof(flowie_server_config),
      .stream_socket_options = CNET_STREAM_SOCKET_OPTIONS_INIT,
      .listener_options = CNET_LISTENER_OPTIONS_INIT,
      .packet = CNET_PACKET_ENDPOINT_CONFIG_INIT
  };
  cnet_client_config net={0};
  broker b={0};
  probe p={0};
  flowie_mqtt_client_t *client=NULL;
  flowie_mqtt_client_config_t cc=FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = {
      .size = sizeof(flowie_mqtt_connect_packet_t),
      .abi_version = FLOWIE_MQTT_PROTOCOL_ABI_V1
  };
  flowie_mqtt_client_topic_handler_t handler = {
      {filter,sizeof(filter)-1u}, legacy_receive};
  flowie_mqtt_client_receive_policy_t policy=FLOWIE_MQTT_CLIENT_RECEIVE_POLICY_INIT;
  uint16_t port=0u;

  atomic_init(&b.pubacks,0u);
  atomic_init(&b.pingreqs,0u);
  atomic_init(&b.connects,0u);
  atomic_init(&b.error,SALTS_OK);
  atomic_init(&p.deliveries,0u);
  atomic_init(&p.legacy_deliveries,0u);
  atomic_init(&p.connected,SALTS_EBUSY);
  atomic_init(&p.pinged,SALTS_EBUSY);
  atomic_init(&p.first_settlement,SALTS_EBUSY);
  atomic_init(&p.second_settlement,SALTS_EBUSY);
  atomic_init(&p.stale_settlement,SALTS_EBUSY);
  atomic_init(&p.callback_self_settlement,SALTS_EBUSY);
  atomic_init(&p.errors,0u);

  net.backend=test_backend();
  net.connection_capacity=4u;net.command_capacity=16u;
  net.request_capacity=8u;net.completion_batch_capacity=8u;
  net.event_capacity=16u;net.max_send_bytes=8192u;
  net.receive_buffer_bytes=8192u;net.tls_io_buffer_bytes=CNET_TLS_MIN_IO_BUFFER_BYTES;
  net.tls_handshake_timeout_ms=TEST_TIMEOUT_MS;
  net.connect_timeout_ms=TEST_TIMEOUT_MS;
  net.read_timeout_ms=TEST_TIMEOUT_MS;net.write_timeout_ms=TEST_TIMEOUT_MS;
  conf.transport=TF_NET_TRANSPORT_TCP;
  conf.host="127.0.0.1";conf.port=0u;conf.stream=net;conf.backlog=4u;
  conf.path="/mqtt";conf.command_capacity=16u;
  conf.command_bytes_capacity=8192u;conf.max_message_bytes=TEST_FRAME_MAX;
  conf.poll_slice_ms=1u;
  b.server=&server;
  conf.observer=(flowie_observer){server_open,server_receive,server_close,NULL,&b};
  assert(flowie_server_init(&server,&conf)==SALTS_OK);
  {
    const int status = flowie_server_start(&server);
    if (status != SALTS_OK)
      fprintf(stderr, "deferred_wire: native Broker start failed status=%d\n", status);
    assert(status == SALTS_OK);
  }
  assert(flowie_server_port(&server,&port)==SALTS_OK && port);

  cc.host="127.0.0.1";cc.port=(int)port;
  cc.timeout_ms=TEST_TIMEOUT_MS;
  cc.topic_handlers=(flowie_mqtt_client_topic_handler_map_t){&handler,1u};
  cc.on_connect=connected;cc.on_ping=pinged;cc.on_error=error_event;
  cc.user_data=&p;
  assert(flowie_mqtt_client_create(&cc,&client)==SALTS_OK && client);
  policy.max_pending_qos1=1u;
  policy.on_delivery=deferred_receive;
  assert(flowie_mqtt_client_set_receive_policy(client,&policy)==SALTS_OK);
  assert(flowie_mqtt_client_set_receive_policy(client,&policy)==SALTS_EBUSY);
  connect.version=FLOWIE_MQTT_VERSION_5;
  connect.clean_start=1u;
  connect.client_id=(flowie_mqtt_span_t){client_id,sizeof(client_id)-1u};
  assert(flowie_mqtt_client_connect(client,&connect)==SALTS_OK);
  assert(wait_status(&p.connected,SALTS_OK));
  assert(wait_uint(&p.deliveries,1u));
  /* Native callback returns promptly; neither normal nor duplicate QoS1
   * is acknowledged until a distinct consumer Owner settles its token. */
  cmeta_sleep_ms(80u);
  assert(atomic_load_explicit(&p.deliveries,memory_order_acquire)==1u);
  assert(atomic_load_explicit(&p.legacy_deliveries,memory_order_acquire)==0u);
  assert(atomic_load_explicit(&p.callback_self_settlement,memory_order_acquire)
         ==SALTS_EBUSY);
  assert(atomic_load_explicit(&b.pubacks,memory_order_acquire)==0u);
  assert(flowie_mqtt_client_ping(client)==SALTS_OK);
  assert(wait_status(&p.pinged,SALTS_OK));
  assert(wait_uint(&b.pingreqs,1u));
  assert(atomic_load_explicit(&b.pubacks,memory_order_acquire)==0u);

  /* An arbitrary session-generation token is queued, but the native
   * Worker must reject it WITHOUT acknowledging or retiring the real
   * delivery. Packet ID alone cannot authorize a settlement. */
  {
    flowie_mqtt_client_inbound_token_t stale=p.token;
    ++stale.session_generation;
    assert(flowie_mqtt_client_settle_inbound_qos1(
        client,&stale,settled_stale,&p)==SALTS_OK);
    assert(wait_status(&p.stale_settlement,SALTS_ENOENT));
    assert(atomic_load_explicit(&b.pubacks,memory_order_acquire)==0u);
    assert(atomic_load_explicit(&p.deliveries,memory_order_acquire)==1u);
  }
  /* A foreign durable Owner would commit its effect/receipt here.
   * We deliberately settle only AFTER verifying native progress and no
   * premature PUBACK; this test doesn't simulate a durable DB. */
  assert(flowie_mqtt_client_settle_inbound_qos1(
      client,&p.token,settled_first,&p)==SALTS_OK);
  assert(wait_status(&p.first_settlement,SALTS_OK));
  assert(wait_uint(&b.pubacks,1u));
  assert(flowie_mqtt_client_settle_inbound_qos1(
      client,&p.token,settled_again,&p)==SALTS_OK);
  assert(wait_status(&p.second_settlement,SALTS_ENOENT));
  cmeta_sleep_ms(50u);
  assert(atomic_load_explicit(&b.pubacks,memory_order_acquire)==1u);
  assert(atomic_load_explicit(&p.errors,memory_order_acquire)==0u);
  assert(atomic_load_explicit(&b.error,memory_order_acquire)==SALTS_OK);

  assert(flowie_mqtt_client_try_destroy(client,TEST_TIMEOUT_MS)==SALTS_OK);
  assert(flowie_server_stop(&server,TEST_TIMEOUT_MS)==SALTS_OK);
  assert(flowie_server_destroy(&server)==SALTS_OK);
  puts("flowie.mqtt.qos1_deferred_wire: PASS native PUBACK only after "
       "external settlement, duplicate bounded, ping progress and stale ACK");
  return 0;
}
