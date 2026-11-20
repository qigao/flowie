#include "flowie_cluster_raft_store_internal.h"

#include "tinytest.h"
#include <core/error.h>
#include <crypto/sha256.h>
#include <file/fs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct flowie_raft_store_capture_s {
  uint64_t command_id;
  uint8_t data[32];
  size_t data_size;
  size_t apply_count;
  size_t snapshot_releases;
  size_t snapshot_restores;
} flowie_raft_store_capture_t;

static void flowie_raft_store_cleanup(const char *path) {
  char generated[SALTS_FS_MAX_PATH];
  size_t segment;
  if (!path)
    return;
  for (segment = 1u; segment <= 4u; ++segment) {
    (void)snprintf(generated, sizeof(generated), "%s.%08zu.wal", path, segment);
    if (fs_access(generated, SALTS_FS_ACCESS_EXISTS) == SALTS_OK)
      check_equal(tt_remove_file(generated), 0);
  }
  (void)snprintf(generated, sizeof(generated), "%s.lock", path);
  if (fs_access(generated, SALTS_FS_ACCESS_EXISTS) == SALTS_OK)
    check_equal(tt_remove_file(generated), 0);
  (void)snprintf(generated, sizeof(generated), "%s.snapshot.1.1", path);
  if (fs_access(generated, SALTS_FS_ACCESS_EXISTS) == SALTS_OK)
    check_equal(tt_remove_file(generated), 0);
  check_equal(tt_remove_file(path), 0);
}

static int flowie_raft_store_apply(void *ctx, const tr_raft_entry_t *entries, size_t entry_count) {
  flowie_raft_store_capture_t *capture = (flowie_raft_store_capture_t *)ctx;
  size_t index;
  if (!capture || !entries || entry_count == 0u)
    return SALTS_EINVAL;
  for (index = 0u; index < entry_count; ++index) {
    if (entries[index].data_length > sizeof(capture->data))
      return SALTS_EMSGSIZE;
    capture->command_id = entries[index].command_id;
    capture->data_size = entries[index].data_length;
    memcpy(capture->data, entries[index].data, entries[index].data_length);
    ++capture->apply_count;
  }
  return SALTS_OK;
}

static int flowie_raft_snapshot_read(void *ctx, uint64_t offset, uint8_t *buffer, size_t capacity,
                                     size_t *out_size) {
  flowie_raft_store_capture_t *capture = (flowie_raft_store_capture_t *)ctx;
  size_t count;
  if (!capture || !buffer || !out_size || offset > capture->data_size)
    return SALTS_EINVAL;
  count = capture->data_size - (size_t)offset;
  if (count > capacity)
    count = capacity;
  memcpy(buffer, capture->data + (size_t)offset, count);
  *out_size = count;
  return SALTS_OK;
}

static void flowie_raft_snapshot_release(void *ctx) {
  ++((flowie_raft_store_capture_t *)ctx)->snapshot_releases;
}

static int flowie_raft_snapshot_create(void *ctx, tr_raft_index_t index,
                                       tr_raft_snapshot_source_t *source) {
  flowie_raft_store_capture_t *capture = (flowie_raft_store_capture_t *)ctx;
  if (!capture || !source || index == 0u || capture->data_size == 0u)
    return SALTS_EINVAL;
  memset(source, 0, sizeof(*source));
  if (salts_crypto_sha256(capture->data, capture->data_size, source->digest) != SALTS_CRYPTO_OK)
    return SALTS_EIO;
  source->context = capture;
  source->size = capture->data_size;
  source->read_at = flowie_raft_snapshot_read;
  source->release = flowie_raft_snapshot_release;
  return SALTS_OK;
}

static int flowie_raft_snapshot_restore(void *ctx, tr_raft_index_t index, tr_raft_term_t term,
                                        const tr_raft_snapshot_source_t *source) {
  flowie_raft_store_capture_t *capture = (flowie_raft_store_capture_t *)ctx;
  uint8_t restored[sizeof(capture->data)];
  size_t used = 0u;
  if (!capture || !source || !source->read_at || index == 0u || term == 0u ||
      source->size > sizeof(restored))
    return SALTS_EINVAL;
  while (used < source->size) {
    size_t count = 0u;
    size_t remaining = (size_t)source->size - used;
    int rc = source->read_at(source->context, used, restored + used, remaining, &count);
    if (rc != SALTS_OK)
      return rc;
    if (count == 0u || count > remaining)
      return SALTS_EPROTO;
    used += count;
  }
  memcpy(capture->data, restored, used);
  capture->data_size = used;
  ++capture->snapshot_restores;
  return SALTS_OK;
}

static flowie_cluster_raft_store_config_t
flowie_raft_store_config(const char *path, const tr_raft_node_id_t *voters,
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

spec("flowie server/cluster TurboRaft durable store") {
  it("restores an owned snapshot source after durable compaction") {
    static const tr_raft_node_id_t voters[] = {1u};
    static const uint8_t command[] = "snapshot-owner";
    char *path = tt_make_temp_file("flowie-raft-snapshot", ".data");
    flowie_raft_store_capture_t first = {0};
    flowie_raft_store_capture_t recovered = {0};
    flowie_cluster_raft_store_config_t config;
    flowie_cluster_raft_store_t *store = NULL;
    tr_raft_proposal_t proposal = {0};
    tr_raft_operation_status_t receipt;
    check_not_null(path);
    config = flowie_raft_store_config(path, voters, &first);
    config.snapshot_threshold = 1u;
    config.snapshot_create = flowie_raft_snapshot_create;
    config.snapshot_create_ctx = &first;
    config.snapshot_restore = flowie_raft_snapshot_restore;
    config.snapshot_restore_ctx = &first;
    check_equal(flowie_cluster_raft_store_open(&config, &store), SALTS_OK);
    check_equal(flowie_cluster_raft_store_tick(store, 3u, 3u), SALTS_OK);
    proposal.command_id = 1u;
    proposal.data = command;
    proposal.data_length = sizeof(command);
    check_equal(flowie_cluster_raft_store_propose(store, &proposal, &receipt), SALTS_OK);
    check_equal(first.snapshot_releases, 1u);
    check_equal(flowie_cluster_raft_store_close(store), SALTS_OK);
    config = flowie_raft_store_config(path, voters, &recovered);
    config.snapshot_restore = flowie_raft_snapshot_restore;
    config.snapshot_restore_ctx = &recovered;
    check_equal(flowie_cluster_raft_store_open(&config, &store), SALTS_OK);
    check_equal(recovered.snapshot_restores, 1u);
    check_equal(recovered.data_size, sizeof(command));
    check_equal(recovered.data, command, sizeof(command));
    check_equal(flowie_cluster_raft_store_close(store), SALTS_OK);
    flowie_raft_store_cleanup(path);
    free(path);
  }

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
    check_equal(flowie_cluster_raft_store_open(&config, &store), SALTS_OK);
    check_equal(flowie_cluster_raft_store_tick(store, 3u, 3u), SALTS_OK);
    proposal.command_id = 41u;
    proposal.data = command;
    proposal.data_length = sizeof(command);
    check_equal(flowie_cluster_raft_store_propose(store, &proposal, &receipt), SALTS_OK);
    check_equal(first.apply_count, 1u);
    check_equal(first.command_id, 41u);
    check_equal(flowie_cluster_raft_store_status(store, &status), SALTS_OK);
    check_equal(status.core.commit_index, 1u);
    check_equal(status.core.applied_index, 1u);
    check_equal(flowie_cluster_raft_store_close(store), SALTS_OK);

    config = flowie_raft_store_config(path, voters, &recovered);
    check_equal(flowie_cluster_raft_store_open(&config, &store), SALTS_OK);
    check_equal(recovered.apply_count, 1u);
    check_equal(recovered.command_id, 41u);
    check_equal(recovered.data_size, sizeof(command));
    check_equal(recovered.data, command, sizeof(command));
    check_equal(flowie_cluster_raft_store_status(store, &status), SALTS_OK);
    check_equal(status.core.commit_index, 1u);
    check_equal(status.core.applied_index, 1u);
    check_equal(flowie_cluster_raft_store_close(store), SALTS_OK);
    flowie_raft_store_cleanup(path);
    free(path);
  }
}
