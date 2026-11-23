#include "flowie.h"
#include "flowie_mqtt_client.h"
#include "flowie_test_cnet.h"
#include "tls_test_pki.h"

#include "tinytest.h"
#include <core/error.h>
#include <threads/threads.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS 10000u
#define FLOWIE_TRANSPORT_SG_BURST_ITEMS 64u

typedef struct flowie_transport_baseline_state_s {
  atomic_int done;
  atomic_int status;
  flowie_mqtt_subscription_t subscription;
  flowie_mqtt_subscribe_packet_t subscribe;
  flowie_mqtt_client_publish_topic_t publish_topic;
  flowie_mqtt_client_publish_topic_vec_t publish;
  flowie_mqtt_span_t unsubscribe_filter;
  flowie_mqtt_unsubscribe_packet_t unsubscribe;
  uint8_t publish_qos;
} flowie_transport_baseline_state_t;

typedef struct flowie_transport_auth_failure_s {
  size_t calls;
  int result;
} flowie_transport_auth_failure_t;

static int flowie_transport_baseline_on_message(flowie_endpoint_core_t *endpoint,
                                                flowie_message_t *message,
                                                flowie_publish_result_t *result, void *ctx) {
  (void)endpoint;
  (void)message;
  (void)ctx;
  if (!result || result->size < sizeof(*result)) return SALTS_EINVAL;
  result->status = SALTS_OK;
  result->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_PROCESSED;
  return SALTS_OK;
}

static int flowie_transport_auth_failure_authenticate(
    void *ctx, const flowie_security_auth_request_t *request,
    flowie_security_principal_t *principal_out) {
  flowie_transport_auth_failure_t *fixture = (flowie_transport_auth_failure_t *)ctx;
  if (!fixture || !request || !principal_out) return SALTS_EINVAL;
  fixture->calls += 1u;
  return fixture->result;
}

static int flowie_transport_expect_connack(flowie_test_cnet_client_t * client,
                                           const uint8_t *expected, size_t expected_size) {
  uint8_t received[8];
  int rc;
  if (!expected || expected_size > sizeof(received)) return SALTS_EINVAL;
  rc = flowie_test_cnet_recv_exact(client, received, expected_size);
  if (rc != SALTS_OK) {
    (void)fprintf(stderr, "CONNACK receive failed: %d\n", rc);
    return SALTS_EPROTO;
  }
  if (memcmp(received, expected, expected_size) != 0) {
    (void)fprintf(stderr, "unexpected CONNACK:");
    for (size_t i = 0u; i < expected_size; ++i) (void)fprintf(stderr, " %02x", received[i]);
    (void)fprintf(stderr, "\n");
    return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int flowie_transport_expect_close(flowie_test_cnet_client_t * client) {
  uint8_t first_byte = 0u;
  int rc;
  if (!flowie_test_cnet_readable(client, 1500u)) return SALTS_OK;
  rc = flowie_test_cnet_recv_exact(client, &first_byte, 1u);
  return rc != SALTS_OK || first_byte == UINT8_C(0xe0) ? SALTS_OK : SALTS_EPROTO;
}

static int flowie_transport_test_connect(flowie_test_cnet_client_t *client, const char *client_id,
                                         int clean_start, flowie_mqtt_span_t properties) {
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  uint8_t encoded[128];
  size_t written;
  if (!client) return SALTS_EIO;
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = (uint8_t)clean_start;
  connect.client_id = (flowie_mqtt_span_t){(const uint8_t *)client_id, strlen(client_id)};
  connect.properties = properties;
  if (flowie_mqtt_connect_packet_encode(&connect, encoded, sizeof(encoded), &written) !=
      FLOWIE_MQTT_PARSE_OK)
    return SALTS_EPROTO;
  return flowie_test_cnet_send(client, encoded, written);
}

static int flowie_transport_expect_packet(flowie_test_cnet_client_t *client, const uint8_t *packet,
                                          size_t size) {
  uint8_t received[16];
  int rc;
  if (size > sizeof(received)) return SALTS_EINVAL;
  rc = flowie_test_cnet_recv_exact(client, received, size);
  if (rc != SALTS_OK) return rc;
  return memcmp(received, packet, size) == 0 ? SALTS_OK : SALTS_EPROTO;
}

static int flowie_transport_fanout(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
                                  flowie_publish_result_t *result, void *ctx) {
  (void)ctx;
  result->status = flowie_endpoint_core_send_message(endpoint, message);
  return result->status;
}

static int flowie_transport_system_topic_case(void) {
  static const uint8_t wildcards[] = {0x82u, 13u, 0u, 1u, 0u, 0u, 1u, '#', 0u,
                                      0u, 3u, '+', '/', '#', 0u};
  static const uint8_t wildcard_ack[] = {0x90u, 5u, 0u, 1u, 0u, 0u, 0u};
  static const uint8_t explicit_sub[] = {0x82u, 12u, 0u, 2u, 0u, 0u, 6u, '$', 'S', 'Y',
                                         'S', '/', '#', 0u};
  static const uint8_t explicit_ack[] = {0x90u, 4u, 0u, 2u, 0u, 0u};
  static const uint8_t version_topic[] = "$SYS/broker/version";
  static const uint8_t publish_denied[] = {0x40u, 4u, 0u, 1u, 0x87u, 0u};
  flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_endpoint_core_t *endpoint = NULL;
  flowie_test_cnet_client_t *client = NULL;
  flowie_mqtt_parse_options_t parse = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_publish_view_t delivered = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
  flowie_mqtt_publish_packet_t write = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
  uint8_t wire[128];
  size_t consumed;
  size_t written;
  int rc;

  config.host = "127.0.0.1";
  config.port = flowie_test_cnet_port();
  if (config.port == 0) return SALTS_EIO;
  config.max_connections = 2u;
  config.max_sessions = 2u;
  config.max_subscriptions_per_session = 4u;
  config.max_inflight_per_session = 4u;
  config.max_packet_size = sizeof(wire);
  config.manage_sessions = 1;
  options.on_message = flowie_transport_fanout;
  rc = flowie_endpoint_core_create("system-topic", &config, &options, &endpoint);
  if (rc == SALTS_OK) rc = flowie_endpoint_core_start(endpoint);
  if (rc != SALTS_OK) goto done;
  client = flowie_test_cnet_connect((uint16_t)config.port);
  rc = flowie_transport_test_connect(client, "system-topic", 1, (flowie_mqtt_span_t){0});
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_mqtt5_connack(client, 0u, 4u, sizeof(wire));
  if (rc == SALTS_OK) rc = flowie_test_cnet_send(client, wildcards, sizeof(wildcards));
  if (rc == SALTS_OK) rc = flowie_transport_expect_packet(client, wildcard_ack, sizeof(wildcard_ack));
  if (rc != SALTS_OK) goto done;
  if (flowie_test_cnet_readable(client, 150u)) { rc = SALTS_EPROTO; goto done; }
  rc = flowie_test_cnet_send(client, explicit_sub, sizeof(explicit_sub));
  if (rc == SALTS_OK) rc = flowie_transport_expect_packet(client, explicit_ack, sizeof(explicit_ack));
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_exact(client, wire, 2u);
  if (rc != SALTS_OK) goto done;
  if (wire[1] > sizeof(wire) - 2u) { rc = SALTS_EPROTO; goto done; }
  rc = flowie_test_cnet_recv_exact(client, wire + 2u, wire[1]);
  if (rc != SALTS_OK) goto done;
  parse.version = FLOWIE_MQTT_VERSION_5;
  if (flowie_mqtt_packet_parse(wire, wire[1] + 2u, &parse, &packet, &consumed, NULL) !=
          FLOWIE_MQTT_PARSE_OK ||
      flowie_mqtt_publish_parse(&packet, &delivered) != FLOWIE_MQTT_PARSE_OK ||
      !delivered.retain || delivered.qos != 0u || delivered.topic.size != sizeof(version_topic) - 1u ||
      memcmp(delivered.topic.data, version_topic, sizeof(version_topic) - 1u) != 0 ||
      delivered.payload.size <= sizeof("Flowie ") - 1u ||
      memcmp(delivered.payload.data, "Flowie ", sizeof("Flowie ") - 1u) != 0) {
    rc = SALTS_EPROTO;
    goto done;
  }

  write.version = FLOWIE_MQTT_VERSION_5;
  write.qos = 1u;
  write.packet_id = 1u;
  write.retain = 1u;
  write.topic = (flowie_mqtt_span_t){version_topic, sizeof(version_topic) - 1u};
  if (flowie_mqtt_publish_packet_encode(&write, wire, sizeof(wire), &written) != FLOWIE_MQTT_PARSE_OK) {
    rc = SALTS_EPROTO;
    goto done;
  }
  rc = flowie_test_cnet_send(client, wire, written);
  if (rc == SALTS_OK)
    rc = flowie_transport_expect_packet(client, publish_denied, sizeof(publish_denied));

done:
  flowie_test_cnet_close(client);
  if (endpoint) {
    const int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == SALTS_OK) rc = stop_rc;
  }
  flowie_endpoint_core_destroy(endpoint);
  return rc;
}

static int flowie_transport_delivery_reconnect_case(void) {
  static const uint8_t subscribe[] = {0x82u, 7u, 0u, 1u, 0u, 0u, 1u, 'q', 2u};
  static const uint8_t suback[] = {0x90u, 4u, 0u, 1u, 0u, 2u};
  static const uint8_t pubrec[] = {0x50u, 2u, 0u, 2u};
  static const uint8_t pubrel[] = {0x62u, 2u, 0u, 2u};
  static const uint8_t pubcomp[] = {0x70u, 2u, 0u, 2u};
  static const uint8_t puback[] = {0x40u, 2u, 0u, 1u};
  static const uint8_t disconnect[] = {0xe0u, 0u};
  uint8_t properties[] = {0x11u, 0u, 0u, 0u, 60u, 0x21u, 0u, 2u};
  flowie_mqtt_span_t connect_properties = {properties, sizeof(properties)};
  uint8_t publish[] = {0x32u, 7u, 0u, 1u, 'q', 0u, 1u, 0u, 'a'};
  flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_endpoint_core_t *endpoint = NULL;
  flowie_test_cnet_client_t *subscriber = NULL;
  flowie_test_cnet_client_t *publisher = NULL;
  const unsigned short port = flowie_test_cnet_port();
  int rc;

  if (port == 0u) return SALTS_EIO;
  config.host = "127.0.0.1";
  config.port = port;
  config.max_connections = 2u;
  config.max_sessions = 2u;
  config.max_subscriptions_per_session = 2u;
  config.max_inflight_per_session = 4u;
  config.max_packet_size = 128u;
  config.manage_sessions = 1;
  options.on_message = flowie_transport_fanout;

  rc = flowie_endpoint_core_create("delivery-reconnect", &config, &options, &endpoint);
  if (rc == SALTS_OK) rc = flowie_endpoint_core_start(endpoint);
  if (rc != SALTS_OK) goto done;
  subscriber = flowie_test_cnet_connect(port);
  publisher = flowie_test_cnet_connect(port);
  rc = flowie_transport_test_connect(subscriber, "subscriber", 1, connect_properties);
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_mqtt5_connack(subscriber, 0u, 4u, 128u);
  if (rc == SALTS_OK)
    rc = flowie_transport_test_connect(publisher, "publisher", 1, (flowie_mqtt_span_t){0});
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_mqtt5_connack(publisher, 0u, 4u, 128u);
  if (rc == SALTS_OK) rc = flowie_test_cnet_send(subscriber, subscribe, sizeof(subscribe));
  if (rc == SALTS_OK) rc = flowie_transport_expect_packet(subscriber, suback, sizeof(suback));
  if (rc != SALTS_OK) goto done;

  for (uint8_t id = 1u; id <= 3u; ++id) {
    uint8_t ack[] = {id == 2u ? 0x50u : 0x40u, 2u, 0u, id};
    publish[0] = id == 2u ? 0x34u : 0x32u;
    publish[6] = id;
    publish[8] = (uint8_t)('a' + id - 1u);
    rc = flowie_test_cnet_send(publisher, publish, sizeof(publish));
    if (rc == SALTS_OK) rc = flowie_transport_expect_packet(publisher, ack, sizeof(ack));
    if (rc == SALTS_OK && id <= 2u)
      rc = flowie_transport_expect_packet(subscriber, publish, sizeof(publish));
    if (rc != SALTS_OK) goto done;
    if (id == 2u) {
      rc = flowie_test_cnet_send(publisher, pubrel, sizeof(pubrel));
      if (rc == SALTS_OK) rc = flowie_transport_expect_packet(publisher, pubcomp, sizeof(pubcomp));
      if (rc == SALTS_OK) rc = flowie_test_cnet_send(subscriber, pubrec, sizeof(pubrec));
      if (rc == SALTS_OK) rc = flowie_transport_expect_packet(subscriber, pubrel, sizeof(pubrel));
      if (rc != SALTS_OK) goto done;
    }
  }
  if (flowie_test_cnet_readable(subscriber, 150u)) {
    rc = SALTS_EPROTO;
    goto done;
  }
  rc = flowie_test_cnet_send(subscriber, disconnect, sizeof(disconnect));
  if (rc == SALTS_OK) rc = flowie_test_cnet_wait(subscriber, &subscriber->closed, 3000u);
  if (rc != SALTS_OK) goto done;
  flowie_test_cnet_close(subscriber);
  subscriber = flowie_test_cnet_connect(port);
  properties[7] = 1u;
  rc = flowie_transport_test_connect(subscriber, "subscriber", 0, connect_properties);
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_mqtt5_connack(subscriber, 1u, 4u, 128u);
  if (rc == SALTS_OK) {
    const uint8_t retransmit[] = {0x3au, 7u, 0u, 1u, 'q', 0u, 1u, 0u, 'a'};
    rc = flowie_transport_expect_packet(subscriber, retransmit, sizeof(retransmit));
  }
  if (rc == SALTS_OK) rc = flowie_transport_expect_packet(subscriber, pubrel, sizeof(pubrel));
  if (rc == SALTS_OK) rc = flowie_test_cnet_send(subscriber, pubcomp, sizeof(pubcomp));
  if (rc != SALTS_OK) goto done;
  if (flowie_test_cnet_readable(subscriber, 150u)) {
    rc = SALTS_EPROTO;
    goto done;
  }
  rc = flowie_test_cnet_send(subscriber, puback, sizeof(puback));
  if (rc == SALTS_OK) rc = flowie_transport_expect_packet(subscriber, publish, sizeof(publish));

done:
  flowie_test_cnet_close(publisher);
  flowie_test_cnet_close(subscriber);
  if (endpoint) {
    int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == SALTS_OK) rc = stop_rc;
  }
  flowie_endpoint_core_destroy(endpoint);
  return rc;
}

typedef enum flowie_transport_policy_case_e {
  FLOWIE_TRANSPORT_KEEP_ALIVE,
  FLOWIE_TRANSPORT_UNSUPPORTED_AUTH,
  FLOWIE_TRANSPORT_INBOUND_SUBSCRIPTION_IDENTIFIER,
  FLOWIE_TRANSPORT_ZERO_TOPIC_ALIAS,
  FLOWIE_TRANSPORT_RECEIVE_MAXIMUM
} flowie_transport_policy_case_t;

static int flowie_transport_connect_policy_case(flowie_transport_policy_case_t policy) {
  static const uint8_t method[] = {
      FLOWIE_MQTT_PROPERTY_AUTHENTICATION_METHOD, 0u, 3u, 't', 's', 't'};
  static const uint8_t rejected[] = {0x20u, 0x03u, 0x00u, 0x8cu, 0x00u};
  static const uint8_t ping[] = {0xc0u, 0x00u};
  static const uint8_t pong[] = {0xd0u, 0x00u};
  static const uint8_t client_id[] = "connect-policy";
  flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_endpoint_core_t *endpoint = NULL;
  flowie_test_cnet_client_t *client = FLOWIE_TEST_INVALID_CNET_CLIENT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  uint8_t outgoing[128];
  uint8_t incoming[3];
  size_t outgoing_size = 0u;
  unsigned short port = flowie_test_cnet_port();
  int rc;

  if (port == 0u) return SALTS_EIO;
  config.host = "127.0.0.1";
  config.port = (int)port;
  config.max_connections = 2u;
  config.max_sessions = 2u;
  config.max_subscriptions_per_session = 2u;
  config.max_inflight_per_session = 4u;
  config.recv_timeout_ms = FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS;
  config.max_packet_size = sizeof(outgoing);
  config.manage_sessions = 1;
  options.on_message = flowie_transport_baseline_on_message;
  rc = flowie_endpoint_core_create("connect-policy", &config, &options, &endpoint);
  if (rc != SALTS_OK) goto done;
  rc = flowie_endpoint_core_start(endpoint);
  if (rc != SALTS_OK) goto done;

  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.keep_alive = policy == FLOWIE_TRANSPORT_KEEP_ALIVE ? 1u : 0u;
  connect.client_id = (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u};
  if (policy == FLOWIE_TRANSPORT_UNSUPPORTED_AUTH)
    connect.properties = (flowie_mqtt_span_t){method, sizeof(method)};
  rc = flowie_mqtt_connect_packet_encode(&connect, outgoing, sizeof(outgoing), &outgoing_size);
  if (rc != FLOWIE_MQTT_PARSE_OK) {
    rc = SALTS_EPROTO;
    goto done;
  }
  client = flowie_test_cnet_connect(port);
  if (client == FLOWIE_TEST_INVALID_CNET_CLIENT) {
    rc = SALTS_EIO;
    goto done;
  }
  rc = flowie_test_cnet_send(client, outgoing, outgoing_size);
  if (rc != SALTS_OK) goto done;
  if (policy == FLOWIE_TRANSPORT_UNSUPPORTED_AUTH) {
    rc = flowie_transport_expect_connack(client, rejected, sizeof(rejected));
    if (rc != SALTS_OK) goto done;
  } else {
    rc = flowie_test_cnet_recv_mqtt5_connack(client, 0u, 4u, sizeof(outgoing));
    if (rc != SALTS_OK) goto done;
    if (policy == FLOWIE_TRANSPORT_INBOUND_SUBSCRIPTION_IDENTIFIER ||
        policy == FLOWIE_TRANSPORT_ZERO_TOPIC_ALIAS) {
      static const uint8_t topic[] = "policy/publish";
      static const uint8_t properties[] = {FLOWIE_MQTT_PROPERTY_SUBSCRIPTION_IDENTIFIER, 1u};
      static const uint8_t alias[] = {FLOWIE_MQTT_PROPERTY_TOPIC_ALIAS, 0u, 1u};
      flowie_mqtt_publish_packet_t publish = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
      publish.version = FLOWIE_MQTT_VERSION_5;
      publish.topic = (flowie_mqtt_span_t){topic, sizeof(topic) - 1u};
      publish.properties = policy == FLOWIE_TRANSPORT_ZERO_TOPIC_ALIAS
                               ? (flowie_mqtt_span_t){alias, sizeof(alias)}
                               : (flowie_mqtt_span_t){properties, sizeof(properties)};
      rc = flowie_mqtt_publish_packet_encode(&publish, outgoing, sizeof(outgoing), &outgoing_size);
      if (rc != FLOWIE_MQTT_PARSE_OK) {
        rc = SALTS_EPROTO;
        goto done;
      }
      if (policy == FLOWIE_TRANSPORT_ZERO_TOPIC_ALIAS) outgoing[outgoing_size - 1u] = 0u;
      rc = flowie_test_cnet_send(client, outgoing, outgoing_size);
      if (rc != SALTS_OK) goto done;
      rc = flowie_test_cnet_recv_exact(client, incoming, sizeof(incoming));
      if (rc != SALTS_OK) goto done;
      if (incoming[0] != 0xe0u ||
          incoming[2] != (policy == FLOWIE_TRANSPORT_ZERO_TOPIC_ALIAS ? 0x94u : 0x82u)) {
        rc = SALTS_EPROTO;
        goto done;
      }
    }
    if (policy == FLOWIE_TRANSPORT_RECEIVE_MAXIMUM) {
      static const uint8_t topic[] = "policy/window";
      flowie_mqtt_publish_packet_t publish = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
      uint8_t burst[128];
      size_t burst_size = 0u;
      publish.version = FLOWIE_MQTT_VERSION_5;
      publish.qos = 2u;
      publish.topic = (flowie_mqtt_span_t){topic, sizeof(topic) - 1u};
      for (uint16_t id = 1u; id <= config.max_inflight_per_session + 1u; ++id) {
        publish.packet_id = id;
        rc = flowie_mqtt_publish_packet_encode(&publish, burst + burst_size,
                                               sizeof(burst) - burst_size, &outgoing_size);
        if (rc != FLOWIE_MQTT_PARSE_OK) {
          rc = SALTS_EPROTO;
          goto done;
        }
        burst_size += outgoing_size;
      }
      rc = flowie_test_cnet_send(client, burst, burst_size);
      if (rc != SALTS_OK) goto done;
      for (uint8_t id = 1u; id <= config.max_inflight_per_session; ++id) {
        const uint8_t expected[] = {0x50u, 0x02u, 0x00u, id};
        uint8_t ack[sizeof(expected)];
        rc = flowie_test_cnet_recv_exact(client, ack, sizeof(ack));
        if (rc != SALTS_OK) goto done;
        if (memcmp(ack, expected, sizeof(ack)) != 0) {
          rc = SALTS_EPROTO;
          goto done;
        }
      }
      rc = flowie_test_cnet_recv_exact(client, incoming, sizeof(incoming));
      if (rc != SALTS_OK) goto done;
      if (incoming[0] != 0xe0u || incoming[2] != 0x93u) {
        rc = SALTS_EPROTO;
        goto done;
      }
    }
    for (size_t i = 0u; policy == FLOWIE_TRANSPORT_KEEP_ALIVE && i < 2u; ++i) {
      if (flowie_test_cnet_readable(client, 850u)) {
        rc = SALTS_EPROTO;
        goto done;
      }
      rc = flowie_test_cnet_send(client, ping, sizeof(ping));
      if (rc != SALTS_OK) goto done;
      rc = flowie_test_cnet_recv_exact(client, incoming, sizeof(pong));
      if (rc != SALTS_OK) goto done;
      if (memcmp(pong, incoming, sizeof(pong)) != 0) {
        rc = SALTS_EPROTO;
        goto done;
      }
    }
  }
  rc = flowie_test_cnet_wait(client, &client->closed, 3500u);

done:
  flowie_test_cnet_close(client);
  if (endpoint) {
    int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == SALTS_OK) rc = stop_rc;
  }
  flowie_endpoint_core_destroy(endpoint);
  return rc;
}

static int flowie_transport_auth_unavailable_case(void) {
  static const uint8_t connack_v5[] = {0x20u, 0x03u, 0x00u, 0x88u, 0x00u};
  static const uint8_t connack_busy_v5[] = {0x20u, 0x03u, 0x00u, 0x89u, 0x00u};
  static const uint8_t connack_v311[] = {0x20u, 0x02u, 0x00u, 0x03u};
  static const struct {
    flowie_mqtt_version_t version;
    int provider_result;
    const uint8_t *connack;
    size_t connack_size;
  } cases[] = {
      {FLOWIE_MQTT_VERSION_5, SALTS_ETIMEDOUT, connack_v5, sizeof(connack_v5)},
      {FLOWIE_MQTT_VERSION_5, SALTS_EIO, connack_v5, sizeof(connack_v5)},
      {FLOWIE_MQTT_VERSION_5, SALTS_EBUSY, connack_busy_v5, sizeof(connack_busy_v5)},
      {FLOWIE_MQTT_VERSION_3_1_1, SALTS_ETIMEDOUT, connack_v311, sizeof(connack_v311)},
      {FLOWIE_MQTT_VERSION_3_1_1, SALTS_EBUSY, connack_v311, sizeof(connack_v311)},
      {FLOWIE_MQTT_VERSION_5, SALTS_EPROTO, NULL, 0u},
  };
  const flowie_execution_binding_t execution = {sizeof(flowie_execution_binding_t),
                                                 FLOWIE_EXECUTION_PRIVATE};
  flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_endpoint_bindings_t bindings = FLOWIE_ENDPOINT_BINDINGS_INIT;
  flowie_endpoint_security_binding_t security = FLOWIE_ENDPOINT_SECURITY_BINDING_INIT;
  flowie_security_realm_config_t realm_config = FLOWIE_SECURITY_REALM_CONFIG_INIT;
  flowie_security_realm_t *realm = NULL;
  flowie_transport_auth_failure_t auth = {0};
  flowie_security_auth_provider_t provider = {
      sizeof(provider), &auth, flowie_transport_auth_failure_authenticate};
  flowie_endpoint_core_t *endpoint = NULL;
  unsigned short port = flowie_test_cnet_port();
  int rc = SALTS_OK;

  if (port == 0u) return SALTS_EIO;
  realm_config.policy_version = 1u;
  rc = flowie_security_realm_create(&realm_config, &realm);
  if (rc != SALTS_OK) goto done;

  security.realm_channel = "security.transport-auth";
  security.auth_method = "password";
  security.auth_provider = &provider;
  security.realm = realm;
  bindings.security = &security;
  config.host = "127.0.0.1";
  config.port = (int)port;
  config.max_connections = 4u;
  config.recv_timeout_ms = FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS;
  config.manage_sessions = 1;
  config.max_sessions = 4u;
  config.max_subscriptions_per_session = 4u;
  config.max_inflight_per_session = 4u;
  options.on_message = flowie_transport_baseline_on_message;
  rc = flowie_endpoint_core_create_ex("transport-auth-unavailable", &config, &options,
                                      &execution, &bindings, &endpoint);
  if (rc != SALTS_OK) goto done;
  rc = flowie_endpoint_core_start(endpoint);
  if (rc != SALTS_OK) goto done;

  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    flowie_test_cnet_client_t * client = FLOWIE_TEST_INVALID_CNET_CLIENT;
    uint8_t connect_packet[128];
    size_t connect_size = 0u;

    connect.version = cases[i].version;
    connect.clean_start = 1u;
    connect.keep_alive = 30u;
    connect.client_id =
        (flowie_mqtt_span_t){(const uint8_t *)"auth-unavailable-client", 23u};
    connect.has_username = 1u;
    connect.has_password = 1u;
    connect.username = (flowie_mqtt_span_t){(const uint8_t *)"writer", 6u};
    connect.password = (flowie_mqtt_span_t){(const uint8_t *)"secret", 6u};
    auth.result = cases[i].provider_result;
    if (flowie_mqtt_connect_packet_encode(&connect, connect_packet, sizeof(connect_packet),
                                           &connect_size) != FLOWIE_MQTT_PARSE_OK) {
      rc = SALTS_EPROTO;
      break;
    }
    client = flowie_test_cnet_connect(port);
    if (client == FLOWIE_TEST_INVALID_CNET_CLIENT) {
      rc = SALTS_EIO;
      break;
    }
    rc = flowie_test_cnet_send(client, connect_packet, connect_size);
    if (rc == SALTS_OK && cases[i].connack)
      rc = flowie_transport_expect_connack(client, cases[i].connack, cases[i].connack_size);
    else if (rc == SALTS_OK)
      rc = flowie_transport_expect_close(client);
    flowie_test_cnet_close(client);
    if (rc != SALTS_OK) {
      (void)fprintf(stderr, "authentication unavailable case %zu failed\n", i);
      break;
    }
  }
  if (rc == SALTS_OK && auth.calls != sizeof(cases) / sizeof(cases[0])) rc = SALTS_EPROTO;

done:
  if (endpoint) {
    int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == SALTS_OK && stop_rc != SALTS_OK) rc = stop_rc;
  }
  flowie_endpoint_core_destroy(endpoint);
  flowie_security_realm_destroy(realm);
  return rc;
}

typedef struct flowie_mtls_probe_s {
  atomic_uint calls;
} flowie_mtls_probe_t;

static int flowie_mtls_authenticate(void *ctx, const flowie_security_auth_request_t *request,
                                    flowie_security_principal_t *principal) {
  flowie_mtls_probe_t *probe = ctx;
  const char *fingerprint = request->peer_certificate_sha256;
  atomic_fetch_add(&probe->calls, 1u);
  if (!fingerprint || strlen(fingerprint) != 64u ||
      strspn(fingerprint, "0123456789abcdef") != 64u) return SALTS_EPERM;
  strcpy(principal->principal_id, "mtls-client");
  strcpy(principal->principal_type, "device");
  strcpy(principal->domain_id, "mqtt-tests");
  strcpy(principal->auth_method, "password");
  principal->scope = FLOWIE_SECURITY_SCOPE_SELF;
  principal->policy_version = 1u;
  return SALTS_OK;
}

static int flowie_mtls_authorize(void *ctx, const flowie_security_request_t *request,
                                 uint64_t now, flowie_security_decision_t *decision) {
  (void)ctx;
  (void)now;
  decision->effect = FLOWIE_SECURITY_ALLOW;
  decision->reason = FLOWIE_SECURITY_REASON_ALLOW_RULE;
  decision->policy_version = request->principal->policy_version;
  return SALTS_OK;
}

static int flowie_transport_mtls_listener_case(void) {
  static const uint8_t ping[] = {0xc0u, 0u};
  static const uint8_t pong[] = {0xd0u, 0u};
  flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_execution_binding_t execution = FLOWIE_EXECUTION_BINDING_INIT;
  flowie_endpoint_bindings_t bindings = FLOWIE_ENDPOINT_BINDINGS_INIT;
  flowie_endpoint_security_binding_t security = FLOWIE_ENDPOINT_SECURITY_BINDING_INIT;
  flowie_security_realm_config_t realm_config = FLOWIE_SECURITY_REALM_CONFIG_INIT;
  flowie_security_realm_t *realm = NULL;
  flowie_endpoint_core_t *endpoint = NULL;
  flowie_test_cnet_client_t *client = NULL;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_mtls_probe_t probe = {0};
  const flowie_security_auth_provider_t auth = {sizeof(auth), &probe, flowie_mtls_authenticate};
  const flowie_security_authorization_provider_t acl = {
      sizeof(acl), NULL, flowie_mtls_authorize};
  cnet_tls_client_config tls = {.size = sizeof(tls)};
  X509_KEY *ca_key = NULL, *client_key = NULL;
  tls_test_certificate *ca = NULL, *cert = NULL;
  char server_ca[512] = {0}, server_cert[512] = {0}, server_key[512] = {0};
  char client_ca[512] = {0}, client_cert[512] = {0}, client_key_file[512] = {0};
  char unknown_cert[512] = {0}, unknown_key[512] = {0};
  uint8_t wire[128];
  size_t written;
  const uint16_t port = flowie_test_cnet_port();
  int rc = SALTS_EIO;

  if (!port || tls_test_write_ca_file(server_ca, sizeof(server_ca)) != 0 ||
      tls_test_write_server_files(server_cert, sizeof(server_cert), server_key, sizeof(server_key)) != 0 ||
      tls_test_set_server_env(server_cert, server_key) != 0 ||
      tls_test_write_temp_file(client_ca, sizeof(client_ca), "ca", "") != 0 ||
      tls_test_write_temp_file(client_cert, sizeof(client_cert), "crt", "") != 0 ||
      tls_test_write_temp_file(client_key_file, sizeof(client_key_file), "key", "") != 0 ||
      tls_test_write_self_signed_files(unknown_cert, sizeof(unknown_cert), unknown_key,
                                        sizeof(unknown_key), "untrusted-mqtt", 0) != 0) goto done;
  ca_key = tls_test_pki_generate_key();
  client_key = tls_test_pki_generate_key();
  ca = tls_test_pki_generate_self_signed(ca_key, "mqtt-client-ca", 1);
  if (!ca || !client_key) goto done;
  cert = tls_test_pki_make_certificate(client_key, ca, ca_key, 2u, "trusted-mqtt", 0, 0,
                                       OID_kp_client_auth, -300L, TLS_TEST_PKI_VALID_SECONDS);
  if (!cert || tls_test_pki_write_certificate(client_ca, ca) != 0 ||
      tls_test_pki_write_certificate(client_cert, cert) != 0 ||
      tls_test_pki_write_key(client_key_file, client_key) != 0) goto done;

  realm_config.policy_source = "acl.remote";
  rc = flowie_security_realm_create(&realm_config, &realm);
  if (rc == SALTS_OK) rc = flowie_security_realm_bind_authorization_provider(realm, &acl);
  if (rc != SALTS_OK) goto done;
  security.realm_channel = "security.mqtt-mtls";
  security.auth_method = "password";
  security.auth_provider = &auth;
  security.realm = realm;
  bindings.security = &security;
  config.host = "127.0.0.1";
  config.port = port;
  config.transport = FLOWIE_TRANSPORT_TLS;
  config.timeout_ms = 2500u;
  config.tls_client_ca_file = client_ca;
  config.max_connections = 4u;
  config.max_sessions = 4u;
  config.max_inflight_per_session = 4u;
  config.max_packet_size = sizeof(wire);
  config.manage_sessions = 1;
  options.on_message = flowie_transport_baseline_on_message;
  rc = flowie_endpoint_core_create_ex("mtls-listener", &config, &options, &execution, &bindings,
                                      &endpoint);
  if (rc == SALTS_OK) rc = flowie_endpoint_core_start(endpoint);
  if (rc != SALTS_OK) goto done;

  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.has_username = 1u;
  connect.client_id = (flowie_mqtt_span_t){(const uint8_t *)"mqtt-mtls", 9u};
  connect.username = (flowie_mqtt_span_t){(const uint8_t *)"mtls-client", 11u};
  if (flowie_mqtt_connect_packet_encode(&connect, wire, sizeof(wire), &written) != FLOWIE_MQTT_PARSE_OK) {
    rc = SALTS_EPROTO;
    goto done;
  }
  tls.ca_file = server_ca;
  for (size_t i = 0u; i < 2u; ++i) {
    if (i != 0u) { tls.cert_file = unknown_cert; tls.key_file = unknown_key; }
    client = flowie_test_cnet_connect_configured(port, 0u, &tls);
    if (client) {
      /* A client can finish its TLS send before the server's certificate rejection arrives. */
      if (flowie_test_cnet_send(client, wire, written) == SALTS_OK &&
          flowie_test_cnet_readable(client, 3000u) && client->received_size != 0u) {
        rc = SALTS_EPROTO;
        goto done;
      }
      flowie_test_cnet_close(client);
      client = NULL;
    }
  }
  if (atomic_load(&probe.calls) != 0u) { rc = SALTS_EPROTO; goto done; }
  tls.cert_file = client_cert;
  tls.key_file = client_key_file;
  client = flowie_test_cnet_connect_configured(port, 0u, &tls);
  if (!client) { rc = SALTS_EIO; goto done; }
  rc = flowie_test_cnet_send(client, wire, written);
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_mqtt5_connack(client, 0u, 4u, sizeof(wire));
  if (rc == SALTS_OK) rc = flowie_test_cnet_send(client, ping, sizeof(ping));
  if (rc == SALTS_OK) rc = flowie_transport_expect_packet(client, pong, sizeof(pong));
  if (rc == SALTS_OK && atomic_load(&probe.calls) != 1u) rc = SALTS_EPROTO;

done:
  flowie_test_cnet_close(client);
  if (endpoint) {
    const int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == SALTS_OK) rc = stop_rc;
  }
  flowie_endpoint_core_destroy(endpoint);
  flowie_security_realm_destroy(realm);
  free(cert);
  free(ca);
  tls_test_pki_free_key(client_key);
  tls_test_pki_free_key(ca_key);
  tls_test_clear_server_env();
  tls_test_remove_file(unknown_key);
  tls_test_remove_file(unknown_cert);
  tls_test_remove_file(client_key_file);
  tls_test_remove_file(client_cert);
  tls_test_remove_file(client_ca);
  tls_test_remove_file(server_key);
  tls_test_remove_file(server_cert);
  tls_test_remove_file(server_ca);
  return rc;
}

static int flowie_transport_sg_burst_case(uint32_t network_workers,
                                          const cnet_tls_client_config *tls) {
  static const uint8_t expected_pingresp[] = {0xd0u, 0x00u};
  flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_endpoint_core_t *endpoint = NULL;
  flowie_test_cnet_client_t *client = FLOWIE_TEST_INVALID_CNET_CLIENT;
  uint8_t connect_packet[128];
  uint8_t pingreq[4];
  uint8_t burst[FLOWIE_TRANSPORT_SG_BURST_ITEMS * 2u];
  uint8_t replies[sizeof(burst)];
  size_t connect_size = 0u;
  size_t pingreq_size = 0u;
  unsigned short port = flowie_test_cnet_port();
  int rc = SALTS_OK;

  if (port == 0u) return SALTS_EIO;
  config.transport = tls ? FLOWIE_TRANSPORT_TLS : FLOWIE_TRANSPORT_TCP;
  config.host = "127.0.0.1";
  config.port = (int)port;
  config.max_connections = network_workers;
  config.network_workers = network_workers;
  if (network_workers > 1u) config.network_policy = FLOWIE_NETWORK_LEAST_CONNECTIONS;
  config.max_packet_size = 4096u;
  config.recv_timeout_ms = FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS;
  config.manage_sessions = 1;
  config.max_sessions = 1u;
  config.max_subscriptions_per_session = 1u;
  config.max_inflight_per_session = FLOWIE_TRANSPORT_SG_BURST_ITEMS;
  options.on_message = flowie_transport_baseline_on_message;

  rc = flowie_endpoint_core_create("transport-sg-burst", &config, &options, &endpoint);
  if (rc != SALTS_OK) goto done;
  rc = flowie_endpoint_core_start(endpoint);
  if (rc != SALTS_OK) goto done;

  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = 1u;
  connect.keep_alive = 30u;
  connect.client_id =
      (flowie_mqtt_span_t){(const uint8_t *)"flowie-sg-burst", sizeof("flowie-sg-burst") - 1u};
  if (flowie_mqtt_connect_packet_encode(&connect, connect_packet, sizeof(connect_packet),
                                        &connect_size) != FLOWIE_MQTT_PARSE_OK) {
    rc = SALTS_EPROTO;
    goto done;
  }
  client = flowie_test_cnet_connect_configured(port, 0u, tls);
  if (client == FLOWIE_TEST_INVALID_CNET_CLIENT) {
    rc = SALTS_EIO;
    goto done;
  }
  rc = flowie_test_cnet_send(client, connect_packet, connect_size);
  if (rc != SALTS_OK) {
    (void)fprintf(stderr, "SG burst CONNECT send failed: %d\n", rc);
    goto done;
  }
  rc = flowie_test_cnet_recv_mqtt5_connack(
      client, 0u, (uint16_t)FLOWIE_TRANSPORT_SG_BURST_ITEMS, 4096u);
  if (rc != SALTS_OK) {
    (void)fprintf(stderr,
                  "SG burst CONNACK failed: rc=%d received=%zu connected=%d closed=%d failed=%d status=%d\n",
                  rc, client->received_size, client->connected, client->closed, client->failed,
                  client->status);
    goto done;
  }

  if (flowie_mqtt_pingreq_encode(FLOWIE_MQTT_VERSION_5, pingreq, sizeof(pingreq),
                                 &pingreq_size) != FLOWIE_MQTT_PARSE_OK ||
      pingreq_size != sizeof(expected_pingresp)) {
    rc = SALTS_EPROTO;
    goto done;
  }
  for (size_t i = 0u; i < FLOWIE_TRANSPORT_SG_BURST_ITEMS; ++i)
    memcpy(burst + i * pingreq_size, pingreq, pingreq_size);

  /*
   * One TCP write gives ingress one buffer containing 64 PINGREQ packets.
   * processing_input keeps the connection reply drain deferred until all 64
   * PINGRESP packets are queued. The endpoint batch limit is 64 while CNet's
   * retained vector limit is 32, so the production path must lower this batch
   * as two logical writes without changing stream order. Each command's small
   * ranges are packed into one contiguous retained range by Flowie::Connection.
   */
  rc = flowie_test_cnet_send(client, burst, sizeof(burst));
  if (rc != SALTS_OK) {
    (void)fprintf(stderr, "SG burst PINGREQ send failed: %d\n", rc);
    goto done;
  }
  rc = flowie_test_cnet_recv_exact(client, replies, sizeof(replies));
  if (rc != SALTS_OK) {
    (void)fprintf(stderr,
                  "SG burst PINGRESP receive failed: rc=%d received=%zu expected=%zu connected=%d closed=%d failed=%d status=%d\n",
                  rc, client->received_size, sizeof(replies), client->connected, client->closed,
                  client->failed, client->status);
    goto done;
  }
  for (size_t i = 0u; i < FLOWIE_TRANSPORT_SG_BURST_ITEMS; ++i) {
    if (memcmp(replies + i * sizeof(expected_pingresp), expected_pingresp,
               sizeof(expected_pingresp)) != 0) {
      rc = SALTS_EPROTO;
      break;
    }
  }

done:
  flowie_test_cnet_close(client);
  if (endpoint) {
    int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == SALTS_OK && stop_rc != SALTS_OK) rc = stop_rc;
  }
  flowie_endpoint_core_destroy(endpoint);
  return rc;
}

static void flowie_transport_baseline_complete(flowie_transport_baseline_state_t *state,
                                               int status) {
  if (status != SALTS_OK) atomic_store_explicit(&state->status, status, memory_order_relaxed);
  atomic_store_explicit(&state->done, 1, memory_order_release);
}

static void flowie_transport_baseline_on_connect(flowie_mqtt_client_t *client, int status,
                                                 const flowie_mqtt_control_packet_view_t *response,
                                                 void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  if (status == SALTS_OK && (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK))
    status = SALTS_EPROTO;
  if (status == SALTS_OK) status = flowie_mqtt_client_subscribe(client, &state->subscribe);
  if (status != SALTS_OK) flowie_transport_baseline_complete(state, status);
}

static void
flowie_transport_baseline_on_subscribe(flowie_mqtt_client_t *client, int status,
                                       const flowie_mqtt_control_packet_view_t *response,
                                       void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  if (status == SALTS_OK && (!response || response->type != FLOWIE_MQTT_PACKET_SUBACK))
    status = SALTS_EPROTO;
  if (status == SALTS_OK) status = flowie_mqtt_client_publish(client, &state->publish);
  if (status != SALTS_OK) flowie_transport_baseline_complete(state, status);
}

static void flowie_transport_baseline_on_publish(flowie_mqtt_client_t *client, int status,
                                                 const flowie_mqtt_control_packet_view_t *response,
                                                 void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  if (status == SALTS_OK) {
    if ((state->publish_qos == 0u && response) ||
        (state->publish_qos == 1u && (!response || response->type != FLOWIE_MQTT_PACKET_PUBACK)) ||
        (state->publish_qos == 2u && (!response || response->type != FLOWIE_MQTT_PACKET_PUBCOMP)))
      status = SALTS_EPROTO;
  }
  if (status == SALTS_OK) status = flowie_mqtt_client_unsubscribe(client, &state->unsubscribe);
  if (status != SALTS_OK) flowie_transport_baseline_complete(state, status);
}

static void
flowie_transport_baseline_on_unsubscribe(flowie_mqtt_client_t *client, int status,
                                         const flowie_mqtt_control_packet_view_t *response,
                                         void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  if (status == SALTS_OK && (!response || response->type != FLOWIE_MQTT_PACKET_UNSUBACK))
    status = SALTS_EPROTO;
  if (status == SALTS_OK) status = flowie_mqtt_client_ping(client);
  if (status != SALTS_OK) flowie_transport_baseline_complete(state, status);
}

static void flowie_transport_baseline_on_ping(flowie_mqtt_client_t *client, int status,
                                              const flowie_mqtt_control_packet_view_t *response,
                                              void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  (void)response;
  if (status == SALTS_OK)
    status = flowie_mqtt_client_disconnect(client, 0u, (flowie_mqtt_span_t){0});
  if (status != SALTS_OK) flowie_transport_baseline_complete(state, status);
}

static void
flowie_transport_baseline_on_disconnect(flowie_mqtt_client_t *client, int status,
                                        const flowie_mqtt_control_packet_view_t *response,
                                        void *user_data) {
  (void)client;
  (void)response;
  flowie_transport_baseline_complete((flowie_transport_baseline_state_t *)user_data, status);
}

static void flowie_transport_baseline_on_error(flowie_mqtt_client_t *client, int status,
                                               void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  (void)client;
  if (!atomic_load_explicit(&state->done, memory_order_acquire))
    flowie_transport_baseline_complete(state, status);
}

static flowie_mqtt_client_transport_t
flowie_transport_baseline_client_transport(flowie_transport_t transport) {
  switch (transport) {
  case FLOWIE_TRANSPORT_TCP:
    return FLOWIE_MQTT_CLIENT_TRANSPORT_TCP;
  case FLOWIE_TRANSPORT_TLS:
    return FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
  case FLOWIE_TRANSPORT_WS:
    return FLOWIE_MQTT_CLIENT_TRANSPORT_WS;
  case FLOWIE_TRANSPORT_WSS:
    return FLOWIE_MQTT_CLIENT_TRANSPORT_WSS;
  default:
    return 0;
  }
}

static int flowie_transport_baseline_case(flowie_transport_t transport,
                                          flowie_mqtt_version_t version, uint8_t qos,
                                          unsigned int client_number, uint32_t network_workers) {
  static const uint8_t filter[] = "transport/baseline";
  static const uint8_t payload[] = "flowie-transport-baseline";
  char client_id[64];
  flowie_endpoint_config_t endpoint_config = FLOWIE_ENDPOINT_CONFIG_INIT;
  flowie_endpoint_core_options_t endpoint_options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
  flowie_transport_baseline_state_t state = {0};
  flowie_mqtt_client_config_t client_config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  flowie_endpoint_core_t *endpoint = NULL;
  flowie_mqtt_client_t *client = NULL;
  unsigned short port = flowie_test_cnet_port();
  uint64_t deadline;
  int rc;

  if (port == 0u) return SALTS_EIO;
  endpoint_config.transport = transport;
  endpoint_config.host = "127.0.0.1";
  endpoint_config.port = (int)port;
  endpoint_config.path = "/mqtt";
  endpoint_config.max_connections = 2u;
  endpoint_config.network_workers = network_workers;
  if (network_workers > 1u) endpoint_config.network_policy = FLOWIE_NETWORK_LEAST_CONNECTIONS;
  endpoint_config.recv_timeout_ms = FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS;
  endpoint_config.manage_sessions = 1;
  endpoint_config.max_sessions = 2u;
  endpoint_config.max_subscriptions_per_session = 2u;
  endpoint_config.max_inflight_per_session = 4u;
  endpoint_config.settlement.qos1 = FLOWIE_PROTOCOL_SETTLE_PROCESSED;
  endpoint_config.settlement.qos2 = FLOWIE_PROTOCOL_SETTLE_PROCESSED;
  endpoint_options.on_message = flowie_transport_baseline_on_message;

  rc = flowie_endpoint_core_create("transport-baseline", &endpoint_config, &endpoint_options,
                                   &endpoint);
  if (rc != SALTS_OK) goto done;
  rc = flowie_endpoint_core_start(endpoint);
  if (rc != SALTS_OK) goto done;

  atomic_init(&state.done, 0);
  atomic_init(&state.status, SALTS_OK);
  state.subscription.filter = (flowie_mqtt_span_t){filter, sizeof(filter) - 1u};
  state.subscription.qos = qos;
  state.subscribe = (flowie_mqtt_subscribe_packet_t)FLOWIE_MQTT_SUBSCRIBE_PACKET_INIT;
  state.subscribe.version = version;
  state.subscribe.subscriptions = &state.subscription;
  state.subscribe.subscription_count = 1u;
  state.publish_topic.qos = qos;
  state.publish_topic.topic = (flowie_mqtt_span_t){filter, sizeof(filter) - 1u};
  state.publish_topic.payload = (flowie_mqtt_span_t){payload, sizeof(payload) - 1u};
  state.publish = (flowie_mqtt_client_publish_topic_vec_t)FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
  state.publish.version = version;
  state.publish.data = &state.publish_topic;
  state.publish.count = 1u;
  state.publish_qos = qos;
  state.unsubscribe_filter = (flowie_mqtt_span_t){filter, sizeof(filter) - 1u};
  state.unsubscribe = (flowie_mqtt_unsubscribe_packet_t)FLOWIE_MQTT_UNSUBSCRIBE_PACKET_INIT;
  state.unsubscribe.version = version;
  state.unsubscribe.filters = &state.unsubscribe_filter;
  state.unsubscribe.filter_count = 1u;

  client_config.transport = flowie_transport_baseline_client_transport(transport);
  if (client_config.transport == 0) {
    rc = SALTS_EINVAL;
    goto done;
  }
  client_config.host = "127.0.0.1";
  client_config.port = (int)port;
  client_config.path = "/mqtt";
  client_config.timeout_ms = FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS;
  client_config.on_connect = flowie_transport_baseline_on_connect;
  client_config.on_subscribe = flowie_transport_baseline_on_subscribe;
  client_config.on_publish = flowie_transport_baseline_on_publish;
  client_config.on_unsubscribe = flowie_transport_baseline_on_unsubscribe;
  client_config.on_ping = flowie_transport_baseline_on_ping;
  client_config.on_disconnect = flowie_transport_baseline_on_disconnect;
  client_config.on_error = flowie_transport_baseline_on_error;
  client_config.user_data = &state;
  if (transport == FLOWIE_TRANSPORT_TLS || transport == FLOWIE_TRANSPORT_WSS) {
    client_config.tls.ca_file = getenv("SALTS_TLS_CA_FILE");
    if (!client_config.tls.ca_file || client_config.tls.ca_file[0] == '\0') {
      rc = SALTS_EINVAL;
      goto done;
    }
  }
  /* Sequential sessions on the same listener exercise adoption on every owner. */
  for (uint32_t owner = 0u; owner < network_workers; ++owner) {
    atomic_store(&state.done, 0);
    atomic_store(&state.status, SALTS_OK);
    rc = flowie_mqtt_client_create(&client_config, &client);
    if (rc != SALTS_OK) goto done;
    rc = flowie_mqtt_client_set_version(client, version);
    if (rc != SALTS_OK) goto done;

    (void)snprintf(client_id, sizeof(client_id), "flowie-baseline-%u-%u", client_number, owner);
    connect.version = version;
    connect.clean_start = 1u;
    connect.keep_alive = 30u;
    connect.client_id = (flowie_mqtt_span_t){(const uint8_t *)client_id, strlen(client_id)};
    rc = flowie_mqtt_client_connect(client, &connect);
    if (rc != SALTS_OK) goto done;

    deadline = monotonic_ms() + FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS;
    while (!atomic_load_explicit(&state.done, memory_order_acquire) &&
           monotonic_ms() < deadline)
      sleep_ms(1u);
    rc = atomic_load_explicit(&state.done, memory_order_acquire)
             ? atomic_load_explicit(&state.status, memory_order_relaxed)
             : SALTS_ETIMEDOUT;
    if (rc != SALTS_OK) goto done;
    flowie_mqtt_client_destroy(client);
    client = NULL;
  }

done:
  flowie_mqtt_client_destroy(client);
  if (endpoint) {
    int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == SALTS_OK && stop_rc != SALTS_OK) rc = stop_rc;
  }
  flowie_endpoint_core_destroy(endpoint);
  return rc;
}

static int flowie_transport_baseline_versions(flowie_transport_t transport,
                                              unsigned int client_number_base,
                                              uint32_t network_workers) {
  static const flowie_mqtt_version_t versions[] = {
      FLOWIE_MQTT_VERSION_3_1, FLOWIE_MQTT_VERSION_3_1_1, FLOWIE_MQTT_VERSION_5};
  for (size_t i = 0u; i < sizeof(versions) / sizeof(versions[0]); ++i) {
    for (uint8_t qos = 0u; qos <= 2u; ++qos) {
      int rc = flowie_transport_baseline_case(transport, versions[i], qos,
                                              client_number_base + (unsigned int)(i * 3u) +
                                                  (unsigned int)qos, network_workers);
      if (rc != SALTS_OK) return rc;
    }
  }
  return SALTS_OK;
}

static int flowie_transport_baseline_secure_versions(flowie_transport_t transport,
                                                     unsigned int client_number_base,
                                                     uint32_t network_workers) {
  char ca_path[512] = {0};
  char cert_path[512] = {0};
  char key_path[512] = {0};
  int rc = SALTS_EIO;
  if (tls_test_write_ca_file(ca_path, sizeof(ca_path)) != 0 ||
      tls_test_write_server_files(cert_path, sizeof(cert_path), key_path, sizeof(key_path)) != 0 ||
      tls_test_set_ca_file_env(ca_path) != 0 || tls_test_set_server_env(cert_path, key_path) != 0)
    goto done;
  rc = flowie_transport_baseline_versions(transport, client_number_base, network_workers);
done:
  tls_test_clear_server_env();
  tls_test_clear_ca_env();
  tls_test_remove_file(key_path);
  tls_test_remove_file(cert_path);
  tls_test_remove_file(ca_path);
  return rc;
}

static int flowie_transport_tls_burst_case(uint32_t network_workers) {
  char ca_path[512] = {0};
  char cert_path[512] = {0};
  char key_path[512] = {0};
  cnet_tls_client_config tls = {.size = sizeof(tls), .ca_file = ca_path};
  int rc = SALTS_EIO;
  if (tls_test_write_ca_file(ca_path, sizeof(ca_path)) != 0 ||
      tls_test_write_server_files(cert_path, sizeof(cert_path), key_path, sizeof(key_path)) != 0 ||
      tls_test_set_server_env(cert_path, key_path) != 0)
    goto done;
  rc = flowie_transport_sg_burst_case(network_workers, &tls);
done:
  tls_test_clear_server_env();
  tls_test_remove_file(key_path);
  tls_test_remove_file(cert_path);
  tls_test_remove_file(ca_path);
  return rc;
}

spec("Flowie TCP/TLS/WS/WSS release baseline") {
  it("requires a trusted inbound client certificate before MQTT authentication") {
    check_equal(flowie_transport_mtls_listener_case(), SALTS_OK);
  }

  it("serves a retained broker version only to explicit system subscriptions and rejects writes") {
    check_equal(flowie_transport_system_topic_case(), SALTS_OK);
  }

  it("replays PUBREL outside Receive Maximum and keeps first-send DUP clear") {
    check_equal(flowie_transport_delivery_reconnect_case(), SALTS_OK);
  }

  it("refreshes CONNECT Keep Alive on MQTT packets and closes an idle connection") {
    check_equal(flowie_transport_connect_policy_case(FLOWIE_TRANSPORT_KEEP_ALIVE), SALTS_OK);
  }

  it("rejects an unsupported Authentication Method in anonymous mode") {
    check_equal(flowie_transport_connect_policy_case(FLOWIE_TRANSPORT_UNSUPPORTED_AUTH), SALTS_OK);
  }

  it("rejects Subscription Identifier in client PUBLISH with Protocol Error") {
    check_equal(flowie_transport_connect_policy_case(
                    FLOWIE_TRANSPORT_INBOUND_SUBSCRIPTION_IDENTIFIER),
                SALTS_OK);
  }

  it("reports Topic Alias Invalid before closing a connection that sent alias zero") {
    check_equal(flowie_transport_connect_policy_case(FLOWIE_TRANSPORT_ZERO_TOPIC_ALIAS), SALTS_OK);
  }

  it("drains admitted PUBREC replies and reports a full Receive Maximum window") {
    check_equal(flowie_transport_connect_policy_case(FLOWIE_TRANSPORT_RECEIVE_MAXIMUM), SALTS_OK);
  }

  it("reports authentication provider unavailability in CONNACK") {
    check_equal(flowie_transport_auth_unavailable_case(), SALTS_OK);
  }

  it("delivers a 64-reply TCP burst through bounded small-packet storage") {
    check_equal(flowie_transport_sg_burst_case(1u, NULL), SALTS_OK);
  }

  it("preserves MQTT reply batching with multiple TCP network owners") {
    check_equal(flowie_transport_sg_burst_case(2u, NULL), SALTS_OK);
  }

  it("delivers a 64-reply TLS burst through bounded small-packet storage") {
    check_equal(flowie_transport_tls_burst_case(1u), SALTS_OK);
  }

  it("preserves small-packet TLS replies with multiple network owners") {
    check_equal(flowie_transport_tls_burst_case(2u), SALTS_OK);
  }

  it("serves MQTT 3.1, 3.1.1, and 5 over TCP") {
    check_equal(flowie_transport_baseline_versions(FLOWIE_TRANSPORT_TCP, 100u, 1u), SALTS_OK);
  }

  it("serves MQTT 3.1, 3.1.1, and 5 over TLS") {
    check_equal(flowie_transport_baseline_secure_versions(FLOWIE_TRANSPORT_TLS, 200u, 1u), SALTS_OK);
  }

  it("serves all MQTT versions and QoS levels with multiple TLS network owners") {
    check_equal(flowie_transport_baseline_secure_versions(FLOWIE_TRANSPORT_TLS, 500u, 2u), SALTS_OK);
  }

  it("serves MQTT 3.1, 3.1.1, and 5 over WS") {
    check_equal(flowie_transport_baseline_versions(FLOWIE_TRANSPORT_WS, 300u, 1u), SALTS_OK);
  }

  it("serves MQTT 3.1, 3.1.1, and 5 over WSS") {
    check_equal(flowie_transport_baseline_secure_versions(FLOWIE_TRANSPORT_WSS, 400u, 1u), SALTS_OK);
  }

  it("serves all MQTT versions and QoS levels with multiple CHttp WS owners") {
    check_equal(flowie_transport_baseline_versions(FLOWIE_TRANSPORT_WS, 600u, 2u), SALTS_OK);
  }

  it("serves all MQTT versions and QoS levels with multiple CHttp WSS owners") {
    check_equal(flowie_transport_baseline_secure_versions(FLOWIE_TRANSPORT_WSS, 700u, 2u), SALTS_OK);
  }
}
