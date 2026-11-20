#ifndef FLOWIE_CLUSTER_STATE_MACHINE_INTERNAL_H
#define FLOWIE_CLUSTER_STATE_MACHINE_INTERNAL_H

#include "flowie_cluster_owner_projection_internal.h"

#include <turboraft/raft_data_stream.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*flowie_cluster_publish_descriptor_apply_fn)(
    void *ctx, tr_raft_index_t index, tr_raft_term_t term,
    uint64_t command_id, const tr_raft_data_descriptor_t *descriptor);

typedef struct flowie_cluster_state_machine_s {
  flowie_cluster_owner_projection_t owners;
  flowie_cluster_publish_descriptor_apply_fn apply_publish;
  void *publish_ctx;
} flowie_cluster_state_machine_t;

int flowie_cluster_state_machine_apply_batch(
    void *ctx, const tr_raft_entry_t *entries, size_t entry_count);

#ifdef __cplusplus
}
#endif

#endif
