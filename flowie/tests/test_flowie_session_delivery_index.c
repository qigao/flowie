#include "flowie_session_internal.h"

#include "tinytest.h"
#include "turbo/clock.h"
#include "turbo_error.h"

#include <string.h>

enum {
  FLOWIE_TEST_DELIVERY_SCALE_COUNT = UINT16_MAX,
  FLOWIE_TEST_DELIVERY_SCALE_MAX_MS = 3000,
  FLOWIE_TEST_DELIVERY_WAIT_ACK = 2,
};

static flowie_mqtt_connect_view_t flowie_test_persistent_connect(const char *client_id) {
  static uint8_t expiry_property[] = {
      FLOWIE_MQTT_PROPERTY_SESSION_EXPIRY_INTERVAL, 0u, 0u, 0u, 60u};
  flowie_mqtt_connect_view_t connect = FLOWIE_MQTT_CONNECT_VIEW_INIT;
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.keep_alive = 30u;
  connect.client_id.data = (const uint8_t *)client_id;
  connect.client_id.size = strlen(client_id);
  connect.properties = (flowie_mqtt_property_block_view_t)FLOWIE_MQTT_PROPERTY_BLOCK_VIEW_INIT;
  connect.properties.values.data = expiry_property;
  connect.properties.values.size = sizeof(expiry_property);
  return connect;
}

static flowie_session_config_t flowie_test_session_config(uint64_t owner_instance_id,
                                                          uint64_t session_id,
                                                          size_t max_inflight) {
  flowie_session_config_t config = FLOWIE_SESSION_CONFIG_INIT;
  config.owner_instance_id = owner_instance_id;
  config.session_id = session_id;
  config.max_subscriptions = 1u;
  config.max_inflight = max_inflight;
  return config;
}

spec("flowie session delivery packet-id index") {
  it("keeps high-cardinality packet-id reservation within the session lookup budget") {
    flowie_session_config_t config = flowie_test_session_config(
        101u, 103u, FLOWIE_TEST_DELIVERY_SCALE_COUNT);
    flowie_mqtt_connect_view_t connect =
        flowie_test_persistent_connect("delivery-scale");
    flowie_session_snapshot_t snapshot = FLOWIE_SESSION_SNAPSHOT_INIT;
    flowie_session_owner_t *owner = flowie_session_owner_create(&config);
    uint64_t started_at;
    uint64_t elapsed_ms;
    size_t reserved = 0u;
    uint16_t packet_id = 0u;
    int rc = TURBO_OK;

    check_not_null(owner);
    check_equal(flowie_session_owner_open(owner, &connect), TURBO_OK);
    started_at = turbo_monotonic_ms();
    for (; reserved < FLOWIE_TEST_DELIVERY_SCALE_COUNT; ++reserved) {
      rc = flowie_session_owner_delivery_reserve(owner, 1u, &packet_id);
      if (rc != TURBO_OK) break;
    }
    elapsed_ms = turbo_monotonic_ms() - started_at;
    info("reserved=%zu elapsed_ms=%llu budget_ms=%u", reserved,
         (unsigned long long)elapsed_ms, FLOWIE_TEST_DELIVERY_SCALE_MAX_MS);
    check_equal(rc, TURBO_OK);
    check_equal(reserved, (size_t)FLOWIE_TEST_DELIVERY_SCALE_COUNT);
    check_equal(flowie_session_owner_snapshot(owner, &snapshot), TURBO_OK);
    check_equal(snapshot.inflight_count, (size_t)FLOWIE_TEST_DELIVERY_SCALE_COUNT);
    check_less_equal((size_t)elapsed_ms, (size_t)FLOWIE_TEST_DELIVERY_SCALE_MAX_MS);
    flowie_session_owner_destroy(owner);
  }

  it("keeps packet-id lookup valid after swap removal and cloning") {
    enum { DELIVERY_COUNT = 8 };
    flowie_session_config_t config =
        flowie_test_session_config(107u, 109u, DELIVERY_COUNT);
    flowie_mqtt_connect_view_t connect =
        flowie_test_persistent_connect("delivery-swap");
    flowie_session_snapshot_t snapshot = FLOWIE_SESSION_SNAPSHOT_INIT;
    flowie_session_owner_t *owner = flowie_session_owner_create(&config);
    flowie_session_owner_t *clone;
    uint16_t packet_ids[DELIVERY_COUNT] = {0};
    static const size_t remaining[] = {0u, 1u, 3u, 4u, 5u, 6u};

    check_not_null(owner);
    check_equal(flowie_session_owner_open(owner, &connect), TURBO_OK);
    for (size_t i = 0u; i < DELIVERY_COUNT; ++i)
      check_equal(flowie_session_owner_delivery_reserve(owner, 1u, &packet_ids[i]),
                  TURBO_OK);
    check_equal(flowie_session_owner_delivery_cancel(owner, packet_ids[2]), TURBO_OK);
    check_equal(flowie_session_owner_delivery_cancel(owner, packet_ids[7]), TURBO_OK);

    clone = flowie_session_owner_clone(owner);
    check_not_null(clone);
    for (size_t i = 0u; i < sizeof(remaining) / sizeof(remaining[0]); ++i)
      check_equal(flowie_session_owner_delivery_cancel(clone, packet_ids[remaining[i]]),
                  TURBO_OK);
    check_equal(flowie_session_owner_snapshot(clone, &snapshot), TURBO_OK);
    check_equal(snapshot.inflight_count, 0u);
    flowie_session_owner_destroy(clone);

    for (size_t i = 0u; i < sizeof(remaining) / sizeof(remaining[0]); ++i)
      check_equal(flowie_session_owner_delivery_cancel(owner, packet_ids[remaining[i]]),
                  TURBO_OK);
    check_equal(flowie_session_owner_snapshot(owner, &snapshot), TURBO_OK);
    check_equal(snapshot.inflight_count, 0u);
    flowie_session_owner_destroy(owner);
  }

  it("rebuilds packet-id lookup from repository deliveries") {
    static const char client_id[] = "delivery-restore";
    static const uint16_t packet_ids[] = {11u, 22u, 33u};
    uint8_t packets[3][9] = {0};
    flowie_protocol_delivery_row_t deliveries[3] = {0};
    flowie_protocol_session_row_t row = FLOWIE_PROTOCOL_SESSION_ROW_INIT;
    flowie_session_config_t config = flowie_test_session_config(113u, 127u, 8u);
    flowie_session_snapshot_t snapshot = FLOWIE_SESSION_SNAPSHOT_INIT;
    flowie_session_owner_t *owner = NULL;

    for (size_t i = 0u; i < sizeof(deliveries) / sizeof(deliveries[0]); ++i) {
      packets[i][0] = 0x32u;
      packets[i][1] = 0x07u;
      packets[i][2] = 0u;
      packets[i][3] = 1u;
      packets[i][4] = 'a';
      packets[i][5] = (uint8_t)(packet_ids[i] >> 8u);
      packets[i][6] = (uint8_t)packet_ids[i];
      packets[i][7] = 0u;
      packets[i][8] = 'x';
      deliveries[i].packet_id = packet_ids[i];
      deliveries[i].qos = 1u;
      deliveries[i].state = FLOWIE_TEST_DELIVERY_WAIT_ACK;
      deliveries[i].packet =
          (flowie_mqtt_span_t){packets[i], sizeof(packets[i])};
    }
    row.client_id =
        (flowie_mqtt_span_t){(const uint8_t *)client_id, sizeof(client_id) - 1u};
    row.revision = 1u;
    row.session_id = 131u;
    row.session_generation = 1u;
    row.mqtt_version = FLOWIE_MQTT_VERSION_5;
    row.keep_alive = 30u;
    row.session_expiry_interval = 60u;
    row.next_delivery_packet_id = packet_ids[2];
    row.deliveries = deliveries;
    row.delivery_count = sizeof(deliveries) / sizeof(deliveries[0]);

    check_equal(flowie_session_owner_repository_restore(&config, &row, &owner), TURBO_OK);
    check_not_null(owner);
    check_equal(flowie_session_owner_delivery_cancel(owner, packet_ids[1]), TURBO_OK);
    check_equal(flowie_session_owner_delivery_cancel(owner, packet_ids[2]), TURBO_OK);
    check_equal(flowie_session_owner_delivery_cancel(owner, packet_ids[0]), TURBO_OK);
    check_equal(flowie_session_owner_snapshot(owner, &snapshot), TURBO_OK);
    check_equal(snapshot.inflight_count, 0u);
    flowie_session_owner_destroy(owner);

    deliveries[1].packet_id = packet_ids[0];
    packets[1][5] = (uint8_t)(packet_ids[0] >> 8u);
    packets[1][6] = (uint8_t)packet_ids[0];
    owner = NULL;
    check_equal(flowie_session_owner_repository_restore(&config, &row, &owner),
                TURBO_EPROTO);
    check_null(owner);
  }
}
