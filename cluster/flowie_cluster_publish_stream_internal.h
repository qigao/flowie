#ifndef FLOWIE_CLUSTER_PUBLISH_STREAM_INTERNAL_H
#define FLOWIE_CLUSTER_PUBLISH_STREAM_INTERNAL_H

#include "flowie_cluster_publish_event_internal.h"

#include <turboraft/raft_data_stream.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_cluster_publish_stream_sender_s
    flowie_cluster_publish_stream_sender_t;
typedef struct flowie_cluster_publish_stream_receiver_s
    flowie_cluster_publish_stream_receiver_t;
typedef struct flowie_cluster_publish_quorum_s flowie_cluster_publish_quorum_t;

typedef int (*flowie_cluster_publish_stream_commit_fn)(
    void *ctx, const flowie_cluster_publish_event_view_t *event);

typedef struct flowie_cluster_publish_stream_sender_config_s {
  tr_raft_node_id_t self_id;
  tr_raft_node_id_t peer_id;
  size_t max_event_bytes;
  size_t chunk_size;
  size_t max_inflight_chunks;
} flowie_cluster_publish_stream_sender_config_t;

typedef struct flowie_cluster_publish_stream_receiver_config_s {
  tr_raft_node_id_t self_id;
  size_t max_event_bytes;
  flowie_cluster_publish_stream_commit_fn commit;
  void *commit_ctx;
} flowie_cluster_publish_stream_receiver_config_t;

int flowie_cluster_publish_stream_sender_create(
    const flowie_cluster_publish_stream_sender_config_t *config,
    flowie_cluster_publish_stream_sender_t **out);
void flowie_cluster_publish_stream_sender_destroy(
    flowie_cluster_publish_stream_sender_t *sender);
int flowie_cluster_publish_stream_sender_begin(
    flowie_cluster_publish_stream_sender_t *sender, tr_raft_term_t term,
    uint64_t stream_id, const void *event, size_t event_size);
int flowie_cluster_publish_stream_sender_next(
    flowie_cluster_publish_stream_sender_t *sender,
    tr_raft_data_chunk_t *out_chunk);
int flowie_cluster_publish_stream_sender_acknowledge(
    flowie_cluster_publish_stream_sender_t *sender,
    const tr_raft_data_ack_t *ack);
int flowie_cluster_publish_stream_sender_cancel(
    flowie_cluster_publish_stream_sender_t *sender, uint64_t stream_offset);
int flowie_cluster_publish_stream_sender_status(
    const flowie_cluster_publish_stream_sender_t *sender,
    tr_raft_data_stream_sender_status_t *out_status);

int flowie_cluster_publish_stream_receiver_create(
    const flowie_cluster_publish_stream_receiver_config_t *config,
    flowie_cluster_publish_stream_receiver_t **out);
void flowie_cluster_publish_stream_receiver_destroy(
    flowie_cluster_publish_stream_receiver_t *receiver);
int flowie_cluster_publish_stream_receiver_handle(
    flowie_cluster_publish_stream_receiver_t *receiver,
    const tr_raft_data_chunk_t *chunk, tr_raft_data_ack_t *out_ack,
    int *out_committed);

int flowie_cluster_publish_quorum_create(
    tr_raft_node_id_t self_id, const tr_raft_conf_t *configuration,
    const tr_raft_data_chunk_t *first_chunk,
    flowie_cluster_publish_quorum_t **out);
void flowie_cluster_publish_quorum_destroy(
    flowie_cluster_publish_quorum_t *quorum);
int flowie_cluster_publish_quorum_mark_local_durable(
    flowie_cluster_publish_quorum_t *quorum);
int flowie_cluster_publish_quorum_acknowledge(
    flowie_cluster_publish_quorum_t *quorum, const tr_raft_data_ack_t *ack);
int flowie_cluster_publish_quorum_make_proposal(
    const flowie_cluster_publish_quorum_t *quorum, uint64_t command_id,
    uint8_t descriptor[TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE],
    tr_raft_proposal_t *out_proposal);

#ifdef __cplusplus
}
#endif

#endif
