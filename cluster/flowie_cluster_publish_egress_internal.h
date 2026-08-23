#ifndef FLOWIE_CLUSTER_PUBLISH_EGRESS_INTERNAL_H
#define FLOWIE_CLUSTER_PUBLISH_EGRESS_INTERNAL_H

#include "flowie_cluster_publish_stream_internal.h"

#include <turboraft/raft_coronet_transport.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_cluster_publish_egress_s flowie_cluster_publish_egress_t;

typedef int (*flowie_cluster_publish_egress_enqueue_fn)(
    void *ctx, const tr_raft_coronet_payload_t *payload);

typedef struct flowie_cluster_publish_egress_config_s {
  tr_raft_node_id_t self_id;
  tr_raft_term_t term;
  uint64_t stream_id;
  tr_raft_conf_t configuration;
  size_t max_event_bytes;
  size_t chunk_size;
  size_t max_inflight_chunks;
  flowie_cluster_publish_egress_enqueue_fn enqueue;
  void *enqueue_ctx;
} flowie_cluster_publish_egress_config_t;

/** On success takes *event and sets it to NULL. */
int flowie_cluster_publish_egress_create(
    const flowie_cluster_publish_egress_config_t *config, tstr *event,
    flowie_cluster_publish_egress_t **out);
void flowie_cluster_publish_egress_destroy(
    flowie_cluster_publish_egress_t *egress);
int flowie_cluster_publish_egress_mark_local_durable(
    flowie_cluster_publish_egress_t *egress);
int flowie_cluster_publish_egress_pump(
    flowie_cluster_publish_egress_t *egress);
int flowie_cluster_publish_egress_acknowledge(
    flowie_cluster_publish_egress_t *egress,
    const tr_raft_data_ack_t *ack);
int flowie_cluster_publish_egress_make_proposal(
    const flowie_cluster_publish_egress_t *egress, uint64_t command_id,
    uint8_t descriptor[TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE],
    tr_raft_proposal_t *out_proposal);

#ifdef __cplusplus
}
#endif

#endif
