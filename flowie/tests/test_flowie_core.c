#include "flowie.h"
#include "flowie_endpoint_owner_lane_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <stdint.h>

static int flowie_test_dispatch(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
                                flowie_publish_result_t *result, void *ctx) {
  (void)endpoint;
  (void)message;
  (void)ctx;
  result->status = SALTS_OK;
  result->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_ACCEPTED;
  return SALTS_OK;
}

suite("Flowie standalone core") {
  group("runtime owner lane identity") {
    it("accepts array members at their stable indices") {
      flowie_endpoint_owner_lane_t lanes[2] = {{.index = 0u}, {.index = 1u}};

      check_true(flowie_owner_lane_is_member(&lanes[0], lanes, 1u));
      check_true(flowie_owner_lane_is_member(&lanes[0], lanes, 2u));
      check_true(flowie_owner_lane_is_member(&lanes[1], lanes, 2u));
    }

    it("rejects a live copy of an owner outside its array") {
      flowie_endpoint_owner_lane_t lanes[1] = {{.index = 0u}};
      flowie_endpoint_owner_lane_t copy = lanes[0];

      check_false(flowie_owner_lane_is_member(&copy, lanes, 1u));
    }

    it("rejects an array member claiming another lane index") {
      flowie_endpoint_owner_lane_t lanes[2] = {{.index = 1u}, {.index = 1u}};

      check_false(flowie_owner_lane_is_member(&lanes[0], lanes, 2u));
    }

    it("rejects out of range indices before indexing the array") {
      flowie_endpoint_owner_lane_t lanes[1] = {{.index = 1u}};

      check_false(flowie_owner_lane_is_member(&lanes[0], lanes, 1u));
      lanes[0].index = SIZE_MAX;
      check_false(flowie_owner_lane_is_member(&lanes[0], lanes, 1u));
    }

    it("rejects missing owners and empty lane storage") {
      flowie_endpoint_owner_lane_t lanes[1] = {{.index = 0u}};

      check_false(flowie_owner_lane_is_member(NULL, lanes, 1u));
      check_false(flowie_owner_lane_is_member(&lanes[0], NULL, 1u));
      check_false(flowie_owner_lane_is_member(&lanes[0], lanes, 0u));
    }
  }

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
