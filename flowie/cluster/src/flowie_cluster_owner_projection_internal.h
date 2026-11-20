#ifndef FLOWIE_CLUSTER_OWNER_PROJECTION_INTERNAL_H
#define FLOWIE_CLUSTER_OWNER_PROJECTION_INTERNAL_H

#include "flowie_cluster_owner_directory_internal.h"

#include <turboraft/raft_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_CLUSTER_OWNER_COMMAND_VERSION 1u
#define FLOWIE_CLUSTER_OWNER_COMMAND_HEADER_SIZE 40u
#define FLOWIE_CLUSTER_OWNER_COMMAND_MAX_SIZE \
  (FLOWIE_CLUSTER_OWNER_COMMAND_HEADER_SIZE + FLOWIE_CLUSTER_NODE_ID_MAX)

typedef enum flowie_cluster_owner_command_kind_e {
  FLOWIE_CLUSTER_OWNER_COMMAND_ASSIGN = 1,
  FLOWIE_CLUSTER_OWNER_COMMAND_REVOKE = 2
} flowie_cluster_owner_command_kind_t;

typedef struct flowie_cluster_owner_command_s {
  flowie_cluster_owner_command_kind_t kind;
  uint32_t shard_id;
  uint64_t owner_epoch;
  size_t node_id_size;
  char node_id[FLOWIE_CLUSTER_NODE_ID_MAX + 1u];
  uint8_t boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE];
} flowie_cluster_owner_command_t;

typedef struct flowie_cluster_owner_projection_s {
  flowie_cluster_owner_directory_t *directory;
} flowie_cluster_owner_projection_t;

int flowie_cluster_owner_command_encode(
    const flowie_cluster_owner_command_t *command, uint8_t *output,
    size_t output_capacity, size_t *output_size);
int flowie_cluster_owner_command_decode(
    const uint8_t *input, size_t input_size,
    flowie_cluster_owner_command_t *out);
int flowie_cluster_owner_projection_apply_batch(
    void *ctx, const tr_raft_entry_t *entries, size_t entry_count);

#ifdef __cplusplus
}
#endif

#endif
