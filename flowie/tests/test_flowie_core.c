#include "flowie.h"

#include "tinytest.h"
#include "turbo_error.h"

static int flowie_test_dispatch(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
                                flowie_publish_result_t *result, void *ctx) {
  (void)endpoint;
  (void)message;
  (void)ctx;
  result->status = TURBO_OK;
  result->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_ACCEPTED;
  return TURBO_OK;
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
    check_int_eq(flowie_endpoint_core_create("standalone", &config, &options, &endpoint),
                 TURBO_OK);
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
    check_int_eq(flowie_message_set_protocol_route(&message, &route), TURBO_OK);
    settlement.message.protocol = FLOWIE_PROTOCOL_MQTT;
    settlement.message.protocol_version = FLOWIE_MQTT_VERSION_5;
    settlement.message.kind = FLOWIE_PROTOCOL_MESSAGE_DATA;
    settlement.message.qos = FLOWIE_PROTOCOL_QOS_1;
    settlement.message.packet_id = 3u;
    settlement.message.session_generation = route.session_generation;
    settlement.requested_point = FLOWIE_PROTOCOL_SETTLE_ACCEPTED;
    check_int_eq(flowie_message_set_protocol_settlement(&message, &settlement), TURBO_OK);
    check_int_eq(flowie_message_complete_protocol_settlement(
                     &message, FLOWIE_PROTOCOL_SETTLE_ACCEPTED),
                 TURBO_OK);
    check_int_eq(flowie_message_complete_protocol_settlement(
                     &message, FLOWIE_PROTOCOL_SETTLE_ACCEPTED),
                 TURBO_EALREADY);
    flowie_message_cleanup(&message);
  }
}
