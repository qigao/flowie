#include "flowie_cluster_publish_egress_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

typedef struct flowie_publish_egress_test_s {
  tr_raft_data_chunk_t chunks[2];
  size_t chunk_count;
} flowie_publish_egress_test_t;

static int flowie_publish_egress_enqueue(
    void *ctx, const tr_raft_transport_payload_t *payload) {
  flowie_publish_egress_test_t *test =
      (flowie_publish_egress_test_t *)ctx;
  if (!test || !payload ||
      payload->kind != TR_RAFT_WIRE_PAYLOAD_DATA_CHUNK ||
      test->chunk_count >= 2u)
    return SALTS_EPROTO;
  test->chunks[test->chunk_count++] = payload->data.data_chunk;
  return SALTS_OK;
}

static tr_raft_data_ack_t flowie_publish_egress_ack(
    const tr_raft_data_chunk_t *chunk) {
  tr_raft_data_ack_t ack;
  memset(&ack, 0, sizeof(ack));
  ack.from = chunk->to;
  ack.to = chunk->from;
  ack.term = chunk->term;
  ack.stream_id = chunk->stream_id;
  ack.stream_size = chunk->stream_size;
  ack.next_offset = chunk->stream_size;
  ack.accepted = true;
  ack.durable = true;
  memcpy(ack.stream_digest, chunk->stream_digest, sizeof(ack.stream_digest));
  return ack;
}

spec("flowie cluster publish egress transfer group") {
  it("transfers event ownership and gates its descriptor on all durable targets") {
    static const uint8_t client_id[] = "publisher-a";
    static const uint8_t packet[] = {0x30u, 0x07u, 0x00u, 0x01u, 'a',
                                     0x00u, 'o',   'k',   '!'};
    uint8_t edge_boot[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {1u};
    tstr event = NULL;
    flowie_cluster_publish_egress_config_t config;
    flowie_cluster_publish_egress_t *egress = NULL;
    flowie_publish_egress_test_t test = {0};
    tr_raft_data_ack_t ack;
    tr_raft_proposal_t proposal;
    uint8_t descriptor[TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE];
    size_t index;

    check_equal(flowie_cluster_publish_event_encode(
                     FLOWIE_MQTT_VERSION_5, FLOWIE_PROTOCOL_SETTLE_RECEIVED,
                     7u, 8u, 9u, 10u, 1000u, vstr_from_cstr("edge-a"),
                     edge_boot,
                     (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u},
                     (flowie_mqtt_span_t){packet, sizeof(packet)}, 4096u,
                     &event),
                 SALTS_OK);
    memset(&config, 0, sizeof(config));
    config.self_id = 1u;
    config.term = 7u;
    config.stream_id = 44u;
    config.configuration.phase = TR_RAFT_CONF_FINAL;
    config.configuration.member_count = 3u;
    for (index = 0u; index < 3u; ++index) {
      config.configuration.members[index].node_id = index + 1u;
      config.configuration.members[index].roles =
          TR_RAFT_CONF_OLD_VOTER | TR_RAFT_CONF_NEW_VOTER;
    }
    config.max_event_bytes = 4096u;
    config.enqueue = flowie_publish_egress_enqueue;
    config.enqueue_ctx = &test;
    check_equal(flowie_cluster_publish_egress_create(&config, &event,
                                                       &egress),
                 SALTS_OK);
    check_null(event);
    check_equal(flowie_cluster_publish_egress_mark_local_durable(egress),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_egress_pump(egress), SALTS_OK);
    check_equal(test.chunk_count, 2u);
    check_not_equal(test.chunks[0].to, test.chunks[1].to);
    check_equal(flowie_cluster_publish_egress_make_proposal(
                     egress, 99u, descriptor, &proposal),
                 SALTS_EBUSY);
    ack = flowie_publish_egress_ack(&test.chunks[0]);
    check_equal(flowie_cluster_publish_egress_acknowledge(egress, &ack),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_egress_make_proposal(
                     egress, 99u, descriptor, &proposal),
                 SALTS_EBUSY);
    ack = flowie_publish_egress_ack(&test.chunks[1]);
    check_equal(flowie_cluster_publish_egress_acknowledge(egress, &ack),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_egress_make_proposal(
                     egress, 99u, descriptor, &proposal),
                 SALTS_OK);
    check_equal(proposal.command_id, 99u);
    check_equal(proposal.data_length,
                  TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE);
    flowie_cluster_publish_egress_destroy(egress);
  }
}
