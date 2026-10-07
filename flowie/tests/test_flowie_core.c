#include "flowie.h"

#include "tinytest.h"
#include "cmeta_error.h"

static int flowie_test_dispatch(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
                                flowie_publish_result_t *result, void *ctx) {
  (void)endpoint;
  (void)message;
  (void)ctx;
  result->status = SALTS_OK;
  result->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_ACCEPTED;
  return SALTS_OK;
}

suite("Flowie public network placement") {
  static flowie_endpoint_core_t *endpoint;

  before_each() { endpoint = NULL; }
  after_each() { flowie_endpoint_core_destroy(endpoint); }

  it("rejects invalid policy, mismatched CPU lists and UDP placement") {
    flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
    flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
    config.host = "127.0.0.1";
    options.on_message = flowie_test_dispatch;
    config.network_policy = (flowie_network_policy_t)2;
    check_equal(flowie_endpoint_core_create("policy", &config, &options, &endpoint), SALTS_EINVAL);
    check_null(endpoint);
    config.network_policy = FLOWIE_NETWORK_ROUND_ROBIN;
    config.network_workers = 2u;
    config.network_cpu_count = 1u;
    check_equal(flowie_endpoint_core_create("cpus", &config, &options, &endpoint), SALTS_EINVAL);
    check_null(endpoint);
    config.network_cpu_count = FLOWIE_MAX_NETWORK_WORKERS + 1u;
    check_equal(flowie_endpoint_core_create("cpus", &config, &options, &endpoint), SALTS_EINVAL);
    config.network_cpu_count = 0u;
    config.network_workers = 1u;
    config.transport = FLOWIE_TRANSPORT_UDP;
    config.network_policy = FLOWIE_NETWORK_LEAST_CONNECTIONS;
    check_equal(flowie_endpoint_core_create("udp", &config, &options, &endpoint), SALTS_ENOTSUP);
    check_null(endpoint);
  }

  it("retains its CPU settings and propagates binding failure from start") {
    flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
    flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
    config.host = "127.0.0.1";
    config.network_cpu_count = 1u;
    config.network_cpus[0] = UINT32_MAX;
    options.on_message = flowie_test_dispatch;
    check_equal(flowie_endpoint_core_create("cpu-failure", &config, &options, &endpoint), SALTS_OK);
    /* The source configuration is no longer authoritative after create. */
    config.network_cpu_count = 0u;
#if defined(_WIN32) || defined(__linux__)
    check_equal(flowie_endpoint_core_start(endpoint), SALTS_ERANGE);
#else
    check_equal(flowie_endpoint_core_start(endpoint), SALTS_ENOTSUP);
#endif
    check_equal(flowie_endpoint_core_stop(endpoint), SALTS_OK);
  }
}

suite("Flowie standalone core") {
  it("creates without an external product composition root") {
    flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
    flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
    flowie_endpoint_core_t *endpoint = NULL;

    config.host = "127.0.0.1";
    config.port = 1883;
    config.manage_sessions = 1;
    options.on_message = flowie_test_dispatch;
    check_equal(flowie_endpoint_core_create("standalone", &config, &options, &endpoint),
                 SALTS_OK);
    check_not_null(endpoint);
    flowie_endpoint_core_destroy(endpoint);
  }

  it("accepts public stream socket tuning for the CNet transport") {
    flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
    flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
    flowie_endpoint_core_t *endpoint = NULL;

    config.host = "127.0.0.1";
    config.port = 1883;
    config.manage_sessions = 1;
    config.reuse_port = 1;
    config.socket_recv_buffer_bytes = 32768u;
    config.socket_send_buffer_bytes = 65536u;
    config.tcp_keepalive = 1;
    config.tcp_keepalive_idle_ms = 60000u;
    config.tcp_keepalive_interval_ms = 10000u;
    config.tcp_keepalive_count = 3u;
    config.linger = 1;
    config.linger_ms = 250u;
    options.on_message = flowie_test_dispatch;

    check_equal(flowie_endpoint_core_create("tuned", &config, &options, &endpoint), SALTS_OK);
    check_not_null(endpoint);
    flowie_endpoint_core_destroy(endpoint);
  }

  it("owns protocol route and settlement metadata") {
    flowie_message_t message;
    flowie_protocol_route_t route = FLOWIE_PROTOCOL_ROUTE_INIT;
    flowie_protocol_settlement_envelope_t settlement =
        FLOWIE_PROTOCOL_SETTLEMENT_ENVELOPE_INIT;

    flowie_message_init(&message);
    route.protocol = FLOWIE_PROTOCOL_MQTT;
    route.owner_instance_id = 7u;
    route.session_id = 11u;
    route.session_generation = 13u;
    check_equal(flowie_message_set_protocol_route(&message, &route), SALTS_OK);
    settlement.message.protocol = FLOWIE_PROTOCOL_MQTT;
    settlement.message.protocol_version = FLOWIE_MQTT_VERSION_5;
    settlement.message.kind = FLOWIE_PROTOCOL_MESSAGE_DATA;
    settlement.message.qos = FLOWIE_PROTOCOL_QOS_1;
    settlement.message.packet_id = 3u;
    settlement.message.session_generation = route.session_generation;
    settlement.requested_point = FLOWIE_PROTOCOL_SETTLE_ACCEPTED;
    check_equal(flowie_message_set_protocol_settlement(&message, &settlement), SALTS_OK);
    check_equal(flowie_message_complete_protocol_settlement(
                     &message, FLOWIE_PROTOCOL_SETTLE_ACCEPTED),
                 SALTS_OK);
    check_equal(flowie_message_complete_protocol_settlement(
                     &message, FLOWIE_PROTOCOL_SETTLE_ACCEPTED),
                 SALTS_EALREADY);
    flowie_message_cleanup(&message);
  }
}
