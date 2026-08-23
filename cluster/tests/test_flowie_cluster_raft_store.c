#include "flowie_cluster_raft_store_internal.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct flowie_raft_store_capture_s {
  uint64_t command_id;
  uint8_t data[32];
  size_t data_size;
  size_t apply_count;
} flowie_raft_store_capture_t;

static void flowie_raft_store_cleanup(const char *path) {
  char generated[TURBO_FS_MAX_PATH];
  size_t segment;
  if (!path) return;
  for (segment = 1u; segment <= 4u; ++segment) {
    (void)snprintf(generated, sizeof(generated), "%s.%08zu.wal", path, segment);
    if (turbo_fs_access(generated, TURBO_FS_ACCESS_EXISTS) == TURBO_OK)
      check_equal(tt_remove_file(generated), 0);
  }
  (void)snprintf(generated, sizeof(generated), "%s.lock", path);
  if (turbo_fs_access(generated, TURBO_FS_ACCESS_EXISTS) == TURBO_OK)
    check_equal(tt_remove_file(generated), 0);
  check_equal(tt_remove_file(path), 0);
}

static int flowie_raft_store_apply(void *ctx, const tr_raft_entry_t *entries,
                                   size_t entry_count) {
  flowie_raft_store_capture_t *capture =
      (flowie_raft_store_capture_t *)ctx;
  size_t index;
  if (!capture || !entries || entry_count == 0u) return TURBO_EINVAL;
  for (index = 0u; index < entry_count; ++index) {
    if (entries[index].data_length > sizeof(capture->data)) return TURBO_EMSGSIZE;
    capture->command_id = entries[index].command_id;
    capture->data_size = entries[index].data_length;
    memcpy(capture->data, entries[index].data, entries[index].data_length);
    ++capture->apply_count;
  }
  return TURBO_OK;
}

static flowie_cluster_raft_store_config_t flowie_raft_store_config(
    const char *path, const tr_raft_node_id_t *voters,
    flowie_raft_store_capture_t *capture) {
  flowie_cluster_raft_store_config_t config;
  memset(&config, 0, sizeof(config));
  config.path = path;
  config.segment_bytes = TR_RAFT_WAL_MIN_SEGMENT_BYTES;
  config.max_transaction_bytes = 8u * 1024u;
  config.max_segments = 4u;
  config.max_snapshot_bytes = 1024u * 1024u;
  config.self_id = voters[0];
  config.voters = voters;
  config.voter_count = 1u;
  config.heartbeat_ticks = 1u;
  config.election_min_ticks = 3u;
  config.election_max_ticks = 5u;
  config.initial_election_timeout_ticks = 3u;
  config.max_log_entries = 32u;
  config.max_inflight_append_requests = 4u;
  config.state_machine.context = capture;
  config.state_machine.apply_batch = flowie_raft_store_apply;
  return config;
}

spec("flowie cluster TurboRaft durable store") {
  it("replays committed state through the application state machine") {
    static const tr_raft_node_id_t voters[] = {1u};
    static const uint8_t command[] = "owner-shard-7";
    char *path = tt_make_temp_file("flowie-raft", ".data");
    flowie_raft_store_capture_t first = {0};
    flowie_raft_store_capture_t recovered = {0};
    flowie_cluster_raft_store_config_t config;
    flowie_cluster_raft_store_t *store = NULL;
    tr_raft_proposal_t proposal;
    tr_raft_operation_status_t receipt;
    tr_raft_service_status_t status;

    check_not_null(path);
    config = flowie_raft_store_config(path, voters, &first);
    check_equal(flowie_cluster_raft_store_open(&config, &store), TURBO_OK);
    check_equal(flowie_cluster_raft_store_tick(store, 3u, 3u), TURBO_OK);
    proposal.command_id = 41u;
    proposal.data = command;
    proposal.data_length = sizeof(command);
    check_equal(flowie_cluster_raft_store_propose(store, &proposal, &receipt),
                 TURBO_OK);
    check_equal(first.apply_count, 1u);
    check_equal(first.command_id, 41u);
    check_equal(flowie_cluster_raft_store_status(store, &status), TURBO_OK);
    check_equal(status.core.commit_index, 1u);
    check_equal(status.core.applied_index, 1u);
    check_equal(flowie_cluster_raft_store_close(store), TURBO_OK);

    config = flowie_raft_store_config(path, voters, &recovered);
    check_equal(flowie_cluster_raft_store_open(&config, &store), TURBO_OK);
    check_equal(recovered.apply_count, 1u);
    check_equal(recovered.command_id, 41u);
    check_equal(recovered.data_size, sizeof(command));
    check_equal(recovered.data, command, sizeof(command));
    check_equal(flowie_cluster_raft_store_status(store, &status), TURBO_OK);
    check_equal(status.core.commit_index, 1u);
    check_equal(status.core.applied_index, 1u);
    check_equal(flowie_cluster_raft_store_close(store), TURBO_OK);
    flowie_raft_store_cleanup(path);
    free(path);
  }
}
