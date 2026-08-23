#ifndef FLOWIE_CLUSTER_RAFT_STORE_INTERNAL_H
#define FLOWIE_CLUSTER_RAFT_STORE_INTERNAL_H

#include <turboraft/raft_service.h>
#include <turboraft/raft_wal_storage.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_cluster_raft_store_s flowie_cluster_raft_store_t;

typedef int (*flowie_cluster_raft_snapshot_restore_fn)(
    void *ctx, tr_raft_index_t index, tr_raft_term_t term,
    const uint8_t *data, size_t size);

typedef struct flowie_cluster_raft_store_config_s {
  const char *path;
  size_t segment_bytes;
  size_t max_transaction_bytes;
  size_t max_segments;
  size_t max_snapshot_bytes;
  tr_raft_node_id_t self_id;
  const tr_raft_node_id_t *voters;
  size_t voter_count;
  uint32_t heartbeat_ticks;
  uint32_t election_min_ticks;
  uint32_t election_max_ticks;
  uint32_t initial_election_timeout_ticks;
  size_t max_log_entries;
  size_t max_inflight_append_requests;
  tr_raft_transport_t transport;
  tr_raft_state_machine_t state_machine;
  tr_raft_index_t snapshot_threshold;
  tr_raft_snapshot_create_fn snapshot_create;
  void *snapshot_create_ctx;
  flowie_cluster_raft_snapshot_restore_fn snapshot_restore;
  void *snapshot_restore_ctx;
} flowie_cluster_raft_store_config_t;

int flowie_cluster_raft_store_open(
    const flowie_cluster_raft_store_config_t *config,
    flowie_cluster_raft_store_t **out);
int flowie_cluster_raft_store_tick(flowie_cluster_raft_store_t *store,
                                   uint32_t elapsed_ticks,
                                   uint32_t next_election_timeout_ticks);
int flowie_cluster_raft_store_step(flowie_cluster_raft_store_t *store,
                                   const tr_raft_message_t *message);
int flowie_cluster_raft_store_propose(
    flowie_cluster_raft_store_t *store, const tr_raft_proposal_t *proposal,
    tr_raft_operation_status_t *out_receipt);
int flowie_cluster_raft_store_status(
    const flowie_cluster_raft_store_t *store,
    tr_raft_service_status_t *out_status);
int flowie_cluster_raft_store_configuration(
    const flowie_cluster_raft_store_t *store,
    tr_raft_conf_t *out_configuration);
int flowie_cluster_raft_store_close(flowie_cluster_raft_store_t *store);

#ifdef __cplusplus
}
#endif

#endif
