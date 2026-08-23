#include "flowie_cluster_publish_ingress_internal.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <string.h>

typedef struct flowie_publish_ingress_test_s {
  size_t commit_count;
  size_t enqueue_count;
  int fail_first_enqueue;
  tr_raft_data_ack_t ack;
} flowie_publish_ingress_test_t;

static int flowie_publish_ingress_commit(
    void *ctx, const flowie_cluster_publish_event_view_t *event) {
  flowie_publish_ingress_test_t *test =
      (flowie_publish_ingress_test_t *)ctx;
  if (!test || !event || event->connection_id != 7u) return TURBO_EPROTO;
  ++test->commit_count;
  return TURBO_OK;
}

static int flowie_publish_ingress_enqueue(
    void *ctx, const tr_raft_coronet_payload_t *payload) {
  flowie_publish_ingress_test_t *test =
      (flowie_publish_ingress_test_t *)ctx;
  if (!test || !payload || payload->kind != TR_RAFT_WIRE_PAYLOAD_DATA_ACK)
    return TURBO_EPROTO;
  ++test->enqueue_count;
  if (test->fail_first_enqueue && test->enqueue_count == 1u)
    return TURBO_ENOSPC;
  test->ack = payload->data.data_ack;
  return TURBO_OK;
}

spec("flowie cluster publish ingress registry") {
  it("retains a committed stream until its durable ACK is enqueued") {
    static const uint8_t client_id[] = "publisher-a";
    static const uint8_t packet[] = {0x30u, 0x07u, 0x00u, 0x01u, 'a',
                                     0x00u, 'o',   'k',   '!'};
    uint8_t edge_boot[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {1u};
    tstr event = NULL;
    flowie_cluster_publish_stream_sender_config_t sender_config = {0};
    flowie_cluster_publish_stream_sender_t *sender = NULL;
    flowie_cluster_publish_ingress_config_t ingress_config = {0};
    flowie_cluster_publish_ingress_t *ingress = NULL;
    flowie_publish_ingress_test_t test = {0};
    tr_raft_coronet_payload_t payload;

    check_equal(flowie_cluster_publish_event_encode(
                     FLOWIE_MQTT_VERSION_5, FLOWIE_PROTOCOL_SETTLE_RECEIVED,
                     7u, 8u, 9u, 10u, 1000u, vstr_from_cstr("edge-a"),
                     edge_boot,
                     (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u},
                     (flowie_mqtt_span_t){packet, sizeof(packet)}, 4096u,
                     &event),
                 TURBO_OK);
    sender_config.self_id = 1u;
    sender_config.peer_id = 2u;
    sender_config.max_event_bytes = 4096u;
    check_equal(flowie_cluster_publish_stream_sender_create(&sender_config,
                                                              &sender),
                 TURBO_OK);
    check_equal(flowie_cluster_publish_stream_sender_begin(
                     sender, 3u, 44u, event, tstr_len(event)),
                 TURBO_OK);
    memset(&payload, 0, sizeof(payload));
    payload.kind = TR_RAFT_WIRE_PAYLOAD_DATA_CHUNK;
    check_equal(flowie_cluster_publish_stream_sender_next(
                     sender, &payload.data.data_chunk),
                 TURBO_OK);

    test.fail_first_enqueue = 1;
    ingress_config.self_id = 2u;
    ingress_config.max_event_bytes = 4096u;
    ingress_config.max_active_streams = 2u;
    ingress_config.commit = flowie_publish_ingress_commit;
    ingress_config.commit_ctx = &test;
    ingress_config.enqueue = flowie_publish_ingress_enqueue;
    ingress_config.enqueue_ctx = &test;
    check_equal(flowie_cluster_publish_ingress_create(&ingress_config,
                                                        &ingress),
                 TURBO_OK);
    check_equal(flowie_cluster_publish_ingress_handle(ingress, &payload),
                 TURBO_ENOSPC);
    check_equal(test.commit_count, 1u);
    check_equal(flowie_cluster_publish_ingress_active_count(ingress), 1u);
    check_equal(flowie_cluster_publish_ingress_handle(ingress, &payload),
                 TURBO_OK);
    check_equal(test.commit_count, 1u);
    check_equal(test.enqueue_count, 2u);
    check_true(test.ack.durable);
    check_equal(test.ack.next_offset, tstr_len(event));
    check_equal(flowie_cluster_publish_ingress_active_count(ingress), 0u);

    flowie_cluster_publish_ingress_destroy(ingress);
    flowie_cluster_publish_stream_sender_destroy(sender);
    tstr_free(event);
  }
}
