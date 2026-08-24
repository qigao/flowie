#include "flowie.h"
#include "flowie_mqtt_client.h"
#include "flowie_test_socket.h"
#include "tls_test_support.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS 10000u

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

static int flowie_transport_baseline_on_message(flowie_endpoint_core_t *endpoint,
                                                flowie_message_t *message,
                                                flowie_publish_result_t *result, void *ctx) {
  (void)endpoint;
  (void)message;
  (void)ctx;
  if (!result || result->size < sizeof(*result)) return TURBO_EINVAL;
  result->status = TURBO_OK;
  result->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_PROCESSED;
  return TURBO_OK;
}

static void flowie_transport_baseline_complete(flowie_transport_baseline_state_t *state,
                                               int status) {
  if (status != TURBO_OK) atomic_store_explicit(&state->status, status, memory_order_relaxed);
  atomic_store_explicit(&state->done, 1, memory_order_release);
}

static void flowie_transport_baseline_on_connect(flowie_mqtt_client_t *client, int status,
                                                 const flowie_mqtt_control_packet_view_t *response,
                                                 void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  if (status == TURBO_OK && (!response || response->type != FLOWIE_MQTT_PACKET_CONNACK))
    status = TURBO_EPROTO;
  if (status == TURBO_OK) status = flowie_mqtt_client_subscribe(client, &state->subscribe);
  if (status != TURBO_OK) flowie_transport_baseline_complete(state, status);
}

static void
flowie_transport_baseline_on_subscribe(flowie_mqtt_client_t *client, int status,
                                       const flowie_mqtt_control_packet_view_t *response,
                                       void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  if (status == TURBO_OK && (!response || response->type != FLOWIE_MQTT_PACKET_SUBACK))
    status = TURBO_EPROTO;
  if (status == TURBO_OK) status = flowie_mqtt_client_publish(client, &state->publish);
  if (status != TURBO_OK) flowie_transport_baseline_complete(state, status);
}

static void flowie_transport_baseline_on_publish(flowie_mqtt_client_t *client, int status,
                                                 const flowie_mqtt_control_packet_view_t *response,
                                                 void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  if (status == TURBO_OK) {
    if ((state->publish_qos == 0u && response) ||
        (state->publish_qos == 1u && (!response || response->type != FLOWIE_MQTT_PACKET_PUBACK)) ||
        (state->publish_qos == 2u && (!response || response->type != FLOWIE_MQTT_PACKET_PUBCOMP)))
      status = TURBO_EPROTO;
  }
  if (status == TURBO_OK) status = flowie_mqtt_client_unsubscribe(client, &state->unsubscribe);
  if (status != TURBO_OK) flowie_transport_baseline_complete(state, status);
}

static void
flowie_transport_baseline_on_unsubscribe(flowie_mqtt_client_t *client, int status,
                                         const flowie_mqtt_control_packet_view_t *response,
                                         void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  if (status == TURBO_OK && (!response || response->type != FLOWIE_MQTT_PACKET_UNSUBACK))
    status = TURBO_EPROTO;
  if (status == TURBO_OK) status = flowie_mqtt_client_ping(client);
  if (status != TURBO_OK) flowie_transport_baseline_complete(state, status);
}

static void flowie_transport_baseline_on_ping(flowie_mqtt_client_t *client, int status,
                                              const flowie_mqtt_control_packet_view_t *response,
                                              void *user_data) {
  flowie_transport_baseline_state_t *state = (flowie_transport_baseline_state_t *)user_data;
  (void)response;
  if (status == TURBO_OK)
    status = flowie_mqtt_client_disconnect(client, 0u, (flowie_mqtt_span_t){0});
  if (status != TURBO_OK) flowie_transport_baseline_complete(state, status);
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
                                          unsigned int client_number) {
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
  unsigned short port = flowie_test_port();
  uint64_t deadline;
  int rc;

  if (port == 0u) return TURBO_EIO;
  endpoint_config.transport = transport;
  endpoint_config.host = "127.0.0.1";
  endpoint_config.port = (int)port;
  endpoint_config.path = "/mqtt";
  endpoint_config.max_connections = 2u;
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
  if (rc != TURBO_OK) goto done;
  rc = flowie_endpoint_core_start(endpoint);
  if (rc != TURBO_OK) goto done;

  atomic_init(&state.done, 0);
  atomic_init(&state.status, TURBO_OK);
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
    rc = TURBO_EINVAL;
    goto done;
  }
  client_config.host = transport == FLOWIE_TRANSPORT_TLS || transport == FLOWIE_TRANSPORT_WSS
                           ? "localhost"
                           : "127.0.0.1";
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
    client_config.tls.ca_file = getenv("TURBONET_TLS_CA_FILE");
    if (!client_config.tls.ca_file || client_config.tls.ca_file[0] == '\0') {
      rc = TURBO_EINVAL;
      goto done;
    }
  }
  rc = flowie_mqtt_client_create(&client_config, &client);
  if (rc != TURBO_OK) goto done;

  (void)snprintf(client_id, sizeof(client_id), "flowie-baseline-%u", client_number);
  connect.version = version;
  connect.clean_start = 1u;
  connect.keep_alive = 30u;
  connect.client_id = (flowie_mqtt_span_t){(const uint8_t *)client_id, strlen(client_id)};
  rc = flowie_mqtt_client_connect(client, &connect);
  if (rc != TURBO_OK) goto done;

  deadline = turbo_monotonic_ms() + FLOWIE_TRANSPORT_BASELINE_TIMEOUT_MS;
  while (!atomic_load_explicit(&state.done, memory_order_acquire) &&
         turbo_monotonic_ms() < deadline)
    turbo_sleep_ms(1u);
  rc = atomic_load_explicit(&state.done, memory_order_acquire)
           ? atomic_load_explicit(&state.status, memory_order_relaxed)
           : TURBO_ETIMEDOUT;

done:
  flowie_mqtt_client_destroy(client);
  if (endpoint) {
    int stop_rc = flowie_endpoint_core_stop(endpoint);
    if (rc == TURBO_OK && stop_rc != TURBO_OK) rc = stop_rc;
  }
  flowie_endpoint_core_destroy(endpoint);
  return rc;
}

static int flowie_transport_baseline_versions(flowie_transport_t transport,
                                              unsigned int client_number_base) {
  static const flowie_mqtt_version_t versions[] = {
      FLOWIE_MQTT_VERSION_3_1, FLOWIE_MQTT_VERSION_3_1_1, FLOWIE_MQTT_VERSION_5};
  for (size_t i = 0u; i < sizeof(versions) / sizeof(versions[0]); ++i) {
    for (uint8_t qos = 0u; qos <= 2u; ++qos) {
      int rc = flowie_transport_baseline_case(transport, versions[i], qos,
                                              client_number_base + (unsigned int)(i * 3u) +
                                                  (unsigned int)qos);
      if (rc != TURBO_OK) return rc;
    }
  }
  return TURBO_OK;
}

static int flowie_transport_baseline_secure_versions(flowie_transport_t transport,
                                                     unsigned int client_number_base) {
  char ca_path[512] = {0};
  char cert_path[512] = {0};
  char key_path[512] = {0};
  int rc = TURBO_EIO;
  if (tls_test_write_ca_file(ca_path, sizeof(ca_path)) != 0 ||
      tls_test_write_server_files(cert_path, sizeof(cert_path), key_path, sizeof(key_path)) != 0 ||
      tls_test_set_ca_file_env(ca_path) != 0 || tls_test_set_server_env(cert_path, key_path) != 0)
    goto done;
  rc = flowie_transport_baseline_versions(transport, client_number_base);
done:
  tls_test_clear_server_env();
  tls_test_clear_ca_env();
  tls_test_remove_file(key_path);
  tls_test_remove_file(cert_path);
  tls_test_remove_file(ca_path);
  return rc;
}

spec("Flowie TCP/TLS/WS/WSS release baseline") {
  it("serves MQTT 3.1, 3.1.1, and 5 over TCP") {
    check_equal(flowie_transport_baseline_versions(FLOWIE_TRANSPORT_TCP, 100u), TURBO_OK);
  }

  it("serves MQTT 3.1, 3.1.1, and 5 over TLS") {
    check_equal(flowie_transport_baseline_secure_versions(FLOWIE_TRANSPORT_TLS, 200u), TURBO_OK);
  }

  it("serves MQTT 3.1, 3.1.1, and 5 over WS") {
    check_equal(flowie_transport_baseline_versions(FLOWIE_TRANSPORT_WS, 300u), TURBO_OK);
  }

  it("serves MQTT 3.1, 3.1.1, and 5 over WSS") {
    check_equal(flowie_transport_baseline_secure_versions(FLOWIE_TRANSPORT_WSS, 400u), TURBO_OK);
  }
}
