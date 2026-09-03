#include "flowie_cluster_peer_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

spec("flowie cluster peer CONNECTION_LOST codec") {
  it("round trips a versioned client identity without MQTT packet bytes") {
    static const uint8_t client_id[] = "device/温度";
    static const uint8_t expected_header[FLOWIE_CLUSTER_PEER_CONNECTION_LOST_HEADER_SIZE] = {
        'T',   'F',   'C',   'L',   0x00u, 0x01u, 0x00u, 0x10u,
        0x00u, 0x00u, 0x00u, 0x1du, 0x00u, 0x0du, 0x05u, 0x00u};
    flowie_cluster_peer_connection_lost_view_t decoded =
        FLOWIE_CLUSTER_PEER_CONNECTION_LOST_VIEW_INIT;
    tstr encoded = NULL;
    check_equal(flowie_cluster_peer_connection_lost_encode(
                     FLOWIE_MQTT_VERSION_5, (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u},
                     128u, &encoded),
                 SALTS_OK);
    check_equal(tstr_len(encoded), sizeof(expected_header) + sizeof(client_id) - 1u);
    check_equal(encoded, expected_header, sizeof(expected_header));
    check_equal(
        flowie_cluster_peer_connection_lost_decode(encoded, tstr_len(encoded), 128u, &decoded),
        SALTS_OK);
    check_equal(decoded.mqtt_version, FLOWIE_MQTT_VERSION_5);
    check_equal(decoded.client_id.data, client_id, decoded.client_id.size);
    tstr_free(encoded);
  }

  it("rejects malformed identities reserved bytes and payload limits") {
    static const uint8_t client_id[] = "device-a";
    static const uint8_t invalid_utf8[] = {0xc0u, 0x80u};
    flowie_cluster_peer_connection_lost_view_t decoded =
        FLOWIE_CLUSTER_PEER_CONNECTION_LOST_VIEW_INIT;
    tstr encoded = NULL;
    check_equal(flowie_cluster_peer_connection_lost_encode(
                     FLOWIE_MQTT_VERSION_5,
                     (flowie_mqtt_span_t){invalid_utf8, sizeof(invalid_utf8)}, 128u, &encoded),
                 SALTS_EPROTO);
    check_null(encoded);
    check_equal(flowie_cluster_peer_connection_lost_encode(
                     FLOWIE_MQTT_VERSION_5, (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u},
                     FLOWIE_CLUSTER_PEER_CONNECTION_LOST_HEADER_SIZE, &encoded),
                 SALTS_EMSGSIZE);
    check_null(encoded);
    check_equal(flowie_cluster_peer_connection_lost_encode(
                     FLOWIE_MQTT_VERSION_5, (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u},
                     128u, &encoded),
                 SALTS_OK);
    encoded[15] = 1u;
    check_equal(
        flowie_cluster_peer_connection_lost_decode(encoded, tstr_len(encoded), 128u, &decoded),
        SALTS_EPROTO);
    encoded[15] = 0u;
    check_equal(flowie_cluster_peer_connection_lost_decode(encoded, tstr_len(encoded),
                                                            tstr_len(encoded) - 1u, &decoded),
                 SALTS_EMSGSIZE);
    tstr_free(encoded);
  }
}
