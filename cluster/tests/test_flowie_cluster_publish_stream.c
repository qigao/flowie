#include "flowie_cluster_publish_stream_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_PUBLISH_STREAM_PAYLOAD_BYTES = 192u * 1024u,
  FLOWIE_PUBLISH_STREAM_MAX_EVENT_BYTES = 512u * 1024u
};

typedef struct flowie_publish_stream_capture_s {
  size_t commit_count;
  size_t packet_size;
  uint64_t connection_id;
} flowie_publish_stream_capture_t;

static size_t flowie_publish_stream_varint(uint32_t value, uint8_t out[4]) {
  size_t used = 0u;
  do {
    uint8_t byte = (uint8_t)(value % 128u);
    value /= 128u;
    if (value != 0u) byte |= 0x80u;
    out[used++] = byte;
  } while (value != 0u);
  return used;
}

static int flowie_publish_stream_commit(
    void *ctx, const flowie_cluster_publish_event_view_t *event) {
  flowie_publish_stream_capture_t *capture =
      (flowie_publish_stream_capture_t *)ctx;
  if (!capture || !event || event->publish.packet.type != FLOWIE_MQTT_PACKET_PUBLISH)
    return SALTS_EPROTO;
  ++capture->commit_count;
  capture->packet_size = event->publish.packet.packet.size;
  capture->connection_id = event->connection_id;
  return SALTS_OK;
}

spec("flowie cluster TurboRaft publish DATA stream") {
  it("gates the 56-byte descriptor proposal on durable quorum") {
    tr_raft_conf_t configuration = {0};
    tr_raft_data_chunk_t first_chunk = {0};
    tr_raft_data_ack_t ack = {0};
    flowie_cluster_publish_quorum_t *quorum = NULL;
    tr_raft_proposal_t proposal;
    uint8_t descriptor[TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE];

    configuration.phase = TR_RAFT_CONF_FINAL;
    configuration.member_count = 3u;
    for (size_t index = 0u; index < 3u; ++index) {
      configuration.members[index].node_id = index + 1u;
      configuration.members[index].roles =
          TR_RAFT_CONF_OLD_VOTER | TR_RAFT_CONF_NEW_VOTER;
    }
    first_chunk.from = 1u;
    first_chunk.to = 2u;
    first_chunk.term = 7u;
    first_chunk.stream_id = 44u;
    first_chunk.stream_size = FLOWIE_PUBLISH_STREAM_PAYLOAD_BYTES;
    first_chunk.data_length = TR_RAFT_WIRE_MAX_DATA_CHUNK_BYTES;
    memset(first_chunk.stream_digest, 0x6au,
           sizeof(first_chunk.stream_digest));
    check_equal(flowie_cluster_publish_quorum_create(
                     1u, &configuration, &first_chunk, &quorum),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_quorum_mark_local_durable(quorum),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_quorum_make_proposal(
                     quorum, 99u, descriptor, &proposal),
                 SALTS_EBUSY);
    ack.from = 2u;
    ack.to = 1u;
    ack.term = 7u;
    ack.stream_id = 44u;
    ack.stream_size = FLOWIE_PUBLISH_STREAM_PAYLOAD_BYTES;
    ack.next_offset = ack.stream_size;
    ack.accepted = true;
    ack.durable = true;
    memcpy(ack.stream_digest, first_chunk.stream_digest,
           sizeof(ack.stream_digest));
    check_equal(flowie_cluster_publish_quorum_acknowledge(quorum, &ack),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_quorum_make_proposal(
                     quorum, 99u, descriptor, &proposal),
                 SALTS_EBUSY);
    ack.from = 3u;
    check_equal(flowie_cluster_publish_quorum_acknowledge(quorum, &ack),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_quorum_make_proposal(
                     quorum, 99u, descriptor, &proposal),
                 SALTS_OK);
    check_equal(proposal.command_id, 99u);
    check_equal(proposal.data_length,
                  TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE);
    check_less_equal(proposal.data_length, TR_RAFT_MAX_ENTRY_BYTES);
    flowie_cluster_publish_quorum_destroy(quorum);
  }

  it("moves a large MQTT publish through bounded 64 KiB chunks") {
    static const uint8_t client_id[] = "publisher-a";
    uint8_t remaining[4];
    const size_t remaining_size =
        flowie_publish_stream_varint(4u + FLOWIE_PUBLISH_STREAM_PAYLOAD_BYTES,
                                     remaining);
    const size_t packet_size =
        1u + remaining_size + 4u + FLOWIE_PUBLISH_STREAM_PAYLOAD_BYTES;
    uint8_t *packet = (uint8_t *)malloc(packet_size);
    uint8_t edge_boot[FLOWIE_CLUSTER_BOOT_ID_SIZE] = {0u};
    tstr event = NULL;
    flowie_cluster_publish_stream_sender_config_t sender_config = {0};
    flowie_cluster_publish_stream_receiver_config_t receiver_config = {0};
    flowie_cluster_publish_stream_sender_t *sender = NULL;
    flowie_cluster_publish_stream_receiver_t *receiver = NULL;
    flowie_publish_stream_capture_t capture = {0};
    tr_raft_data_stream_sender_status_t status;
    size_t chunk_count = 0u;
    int committed = 0;

    check_not_null(packet);
    packet[0] = 0x30u;
    memcpy(packet + 1u, remaining, remaining_size);
    packet[1u + remaining_size] = 0u;
    packet[2u + remaining_size] = 1u;
    packet[3u + remaining_size] = 'a';
    packet[4u + remaining_size] = 0u;
    memset(packet + 5u + remaining_size, 0x5cu,
           FLOWIE_PUBLISH_STREAM_PAYLOAD_BYTES);
    edge_boot[0] = 1u;
    check_equal(flowie_cluster_publish_event_encode(
                     FLOWIE_MQTT_VERSION_5, FLOWIE_PROTOCOL_SETTLE_RECEIVED,
                     7u, 8u, 9u, 10u, 1000u, vstr_from_cstr("edge-a"),
                     edge_boot,
                     (flowie_mqtt_span_t){client_id, sizeof(client_id) - 1u},
                     (flowie_mqtt_span_t){packet, packet_size},
                     FLOWIE_PUBLISH_STREAM_MAX_EVENT_BYTES, &event),
                 SALTS_OK);

    sender_config.self_id = 1u;
    sender_config.peer_id = 2u;
    sender_config.max_event_bytes = FLOWIE_PUBLISH_STREAM_MAX_EVENT_BYTES;
    receiver_config.self_id = 2u;
    receiver_config.max_event_bytes = FLOWIE_PUBLISH_STREAM_MAX_EVENT_BYTES;
    receiver_config.commit = flowie_publish_stream_commit;
    receiver_config.commit_ctx = &capture;
    check_equal(flowie_cluster_publish_stream_sender_create(&sender_config,
                                                              &sender),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_stream_receiver_create(&receiver_config,
                                                                &receiver),
                 SALTS_OK);
    check_equal(flowie_cluster_publish_stream_sender_begin(
                     sender, 3u, 44u, event, tstr_len(event)),
                 SALTS_OK);

    while (!committed) {
      tr_raft_data_chunk_t chunk;
      tr_raft_data_ack_t ack;
      check_equal(flowie_cluster_publish_stream_sender_next(sender, &chunk),
                   SALTS_OK);
      check_less_equal(chunk.data_length,
                    TR_RAFT_WIRE_MAX_DATA_CHUNK_BYTES);
      ++chunk_count;
      check_equal(flowie_cluster_publish_stream_receiver_handle(
                       receiver, &chunk, &ack, &committed),
                   SALTS_OK);
      check_equal(flowie_cluster_publish_stream_sender_acknowledge(sender,
                                                                    &ack),
                   SALTS_OK);
    }
    check_equal(capture.commit_count, 1u);
    check_equal(capture.packet_size, packet_size);
    check_equal(capture.connection_id, 7u);
    check_equal(chunk_count, 4u);
    check_equal(flowie_cluster_publish_stream_sender_status(sender, &status),
                 SALTS_OK);
    check_true(status.complete);
    check_equal(status.acknowledged_offset, tstr_len(event));

    flowie_cluster_publish_stream_receiver_destroy(receiver);
    flowie_cluster_publish_stream_sender_destroy(sender);
    tstr_free(event);
    free(packet);
  }
}
