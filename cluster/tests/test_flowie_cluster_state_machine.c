#include "flowie_cluster_state_machine_internal.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <string.h>

typedef struct flowie_state_machine_capture_s {
  size_t count;
  tr_raft_index_t index;
  tr_raft_term_t term;
  uint64_t command_id;
  tr_raft_data_descriptor_t descriptor;
} flowie_state_machine_capture_t;

static int flowie_state_machine_publish(
    void *ctx, tr_raft_index_t index, tr_raft_term_t term,
    uint64_t command_id, const tr_raft_data_descriptor_t *descriptor) {
  flowie_state_machine_capture_t *capture =
      (flowie_state_machine_capture_t *)ctx;
  if (!capture || !descriptor) return TURBO_EINVAL;
  ++capture->count;
  capture->index = index;
  capture->term = term;
  capture->command_id = command_id;
  capture->descriptor = *descriptor;
  return TURBO_OK;
}

static flowie_cluster_owner_directory_t *flowie_state_machine_directory(void) {
  flowie_cluster_owner_directory_config_t config =
      FLOWIE_CLUSTER_OWNER_DIRECTORY_CONFIG_INIT;
  flowie_cluster_owner_directory_t *directory = NULL;
  config.shard_count = 2u;
  config.cluster_id = vstr_from_cstr("cluster-a");
  config.listener_id = vstr_from_cstr("mqtt");
  check_equal(flowie_cluster_owner_directory_create(&config, &directory),
               TURBO_OK);
  return directory;
}

spec("flowie cluster unified Raft state machine") {
  it("dispatches owner commands and DATA descriptors from one committed batch") {
    flowie_cluster_owner_directory_t *directory =
        flowie_state_machine_directory();
    flowie_state_machine_capture_t capture = {0};
    flowie_cluster_state_machine_t state = {
        {directory}, flowie_state_machine_publish, &capture};
    flowie_cluster_owner_command_t owner_command = {0};
    flowie_cluster_owner_token_t owner = FLOWIE_CLUSTER_OWNER_TOKEN_INIT;
    tr_raft_data_descriptor_t descriptor = {0};
    tr_raft_entry_t entries[2] = {0};
    size_t encoded_size = 0u;

    owner_command.kind = FLOWIE_CLUSTER_OWNER_COMMAND_ASSIGN;
    owner_command.shard_id = 1u;
    owner_command.owner_epoch = 9u;
    owner_command.node_id_size = strlen("node-b");
    memcpy(owner_command.node_id, "node-b", owner_command.node_id_size);
    owner_command.boot_id[0] = 4u;
    entries[0].index = 10u;
    entries[0].term = 3u;
    entries[0].command_id = 80u;
    check_equal(flowie_cluster_owner_command_encode(
                     &owner_command, entries[0].data, sizeof(entries[0].data),
                     &entries[0].data_length),
                 TURBO_OK);

    descriptor.stream_id = 71u;
    descriptor.stream_size = 4096u;
    descriptor.stream_digest[0] = 5u;
    entries[1].index = 11u;
    entries[1].term = 3u;
    entries[1].command_id = 81u;
    check_equal(tr_raft_data_descriptor_encode(
                     &descriptor, entries[1].data, sizeof(entries[1].data),
                     &encoded_size),
                 TURBO_OK);
    entries[1].data_length = encoded_size;

    check_equal(flowie_cluster_state_machine_apply_batch(&state, entries, 2u),
                 TURBO_OK);
    check_equal(flowie_cluster_owner_directory_resolve_shard(directory, 1u,
                                                               &owner),
                 TURBO_OK);
    check_equal(owner.owner_epoch, 9u);
    check_equal(capture.count, 1u);
    check_equal(capture.index, 11u);
    check_equal(capture.term, 3u);
    check_equal(capture.command_id, 81u);
    check_equal(capture.descriptor.stream_id, 71u);
    flowie_cluster_owner_directory_destroy(directory);
  }

  it("rejects an unknown committed command namespace") {
    flowie_cluster_owner_directory_t *directory =
        flowie_state_machine_directory();
    flowie_state_machine_capture_t capture = {0};
    flowie_cluster_state_machine_t state = {
        {directory}, flowie_state_machine_publish, &capture};
    tr_raft_entry_t entry = {0};
    memcpy(entry.data, "NOPE", 4u);
    entry.data_length = 4u;
    entry.index = 1u;
    check_equal(flowie_cluster_state_machine_apply_batch(&state, &entry, 1u),
                 TURBO_EPROTO);
    check_equal(capture.count, 0u);
    flowie_cluster_owner_directory_destroy(directory);
  }
}
