#include "flowie_cluster_publish_router_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

typedef struct flowie_publish_router_test_s {
  tr_raft_data_chunk_t chunks[2];
  size_t chunk_count;
  size_t proposal_count;
  uint64_t command_id;
  size_t proposal_size;
} flowie_publish_router_test_t;

static int flowie_publish_router_commit(
    void *ctx, const flowie_cluster_publish_event_view_t *event) {
  (void)ctx;
  return event ? SALTS_OK : SALTS_EINVAL;
}

static int flowie_publish_router_enqueue(
    void *ctx, const tr_raft_transport_payload_t *payload) {
  flowie_publish_router_test_t *test =
      (flowie_publish_router_test_t *)ctx;
  if (!test || !payload ||
      payload->kind != TR_RAFT_WIRE_PAYLOAD_DATA_CHUNK ||
      test->chunk_count >= 2u)
    return SALTS_EPROTO;
  test->chunks[test->chunk_count++] = payload->data.data_chunk;
  return SALTS_OK;
}

static int flowie_publish_router_propose(void *ctx,
                                         const tr_raft_proposal_t *proposal) {
  flowie_publish_router_test_t *test =
      (flowie_publish_router_test_t *)ctx;
  if (!test || !proposal) return SALTS_EINVAL;
  ++test->proposal_count;
  test->command_id = proposal->command_id;
  test->proposal_size = proposal->data_length;
  return SALTS_OK;
}

static tr_raft_transport_payload_t flowie_publish_router_ack(
    const tr_raft_data_chunk_t *chunk) {
  tr_raft_transport_payload_t payload;
  memset(&payload, 0, sizeof(payload));
  payload.kind = TR_RAFT_WIRE_PAYLOAD_DATA_ACK;
  payload.data.data_ack.from = chunk->to;
  payload.data.data_ack.to = chunk->from;
  payload.data.data_ack.term = chunk->term;
  payload.data.data_ack.stream_id = chunk->stream_id;
  payload.data.data_ack.stream_size = chunk->stream_size;
  payload.data.data_ack.next_offset = chunk->stream_size;
  payload.data.data_ack.accepted = true;
  payload.data.data_ack.durable = true;
  memcpy(payload.data.data_ack.stream_digest, chunk->stream_digest,
         sizeof(payload.data.data_ack.stream_digest));
  return payload;
}

spec("flowie cluster publish router") {
  it("submits one descriptor after routing all durable ACKs") {
    static const uint8_t client_id[] = "publisher-a";
    static const uint8_t packet[] = {0x30u, 0x07u, 0x00u, 0x01u, 'a',
                                     0x00u, 'o',   'k',   '!'};
    uint8_t edge_boot[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {1u};
    tstr event = NULL;
    flowie_cluster_publish_router_config_t router_config = {0};
    flowie_cluster_publish_router_t *router = NULL;
    flowie_publish_router_test_t test = {0};
    tr_raft_conf_t configuration = {0};
    tr_raft_transport_payload_t ack;
    size_t index;

    check_equal(flowie_cluster_publish_event_encode(
                     FLOWIE_MQTT_VERSION_5, FLOWIE_PROTOCOL_SETTLE_RECEIVED,
                     7u, 8u, 9u, 10u, 1000u, vstr_from_cstr("edge-a"),
                     edge_boot,
                     (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u},
                     (flowie_mqtt_span_t){packet, sizeof(packet)}, 4096u,
                     &event),
                 SALTS_OK);
    router_config.self_id = 1u;
    router_config.max_event_bytes = 4096u;
    router_config.max_inbound_streams = 4u;
    router_config.max_outbound_streams = 4u;
    router_config.commit = flowie_publish_router_commit;
    router_config.enqueue = flowie_publish_router_enqueue;
    router_config.enqueue_ctx = &test;
    router_config.propose = flowie_publish_router_propose;
    router_config.propose_ctx = &test;
    check_equal(flowie_cluster_publish_router_create(&router_config,
                                                       &router),
                 SALTS_OK);
    configuration.phase = TR_RAFT_CONF_FINAL;
    configuration.member_count = 3u;
    for (index = 0u; index < 3u; ++index) {
      configuration.members[index].node_id = index + 1u;
      configuration.members[index].roles =
          TR_RAFT_CONF_OLD_VOTER | TR_RAFT_CONF_NEW_VOTER;
    }
    check_equal(flowie_cluster_publish_router_submit_durable(
                     router, 7u, 44u, 99u, &configuration, &event),
                 SALTS_OK);
    check_null(event);
    check_equal(test.chunk_count, 2u);
    check_equal(flowie_cluster_publish_router_outbound_count(router), 1u);
    ack = flowie_publish_router_ack(&test.chunks[0]);
    check_equal(flowie_cluster_publish_router_handle(router, &ack), SALTS_OK);
    check_equal(test.proposal_count, 0u);
    check_equal(flowie_cluster_publish_router_outbound_count(router), 1u);
    ack = flowie_publish_router_ack(&test.chunks[1]);
    check_equal(flowie_cluster_publish_router_handle(router, &ack), SALTS_OK);
    check_equal(test.proposal_count, 1u);
    check_equal(test.command_id, 99u);
    check_equal(test.proposal_size, TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE);
    check_equal(flowie_cluster_publish_router_outbound_count(router), 0u);
    check_equal(flowie_cluster_publish_router_destroy(router), SALTS_OK);
  }
}
