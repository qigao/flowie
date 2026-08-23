#include "flowie_cluster_raft_store_internal.h"

#include "turbo_error.h"

#include <stdlib.h>
#include <string.h>

struct flowie_cluster_raft_store_s {
  tr_raft_wal_storage_t *storage;
  tr_raft_service_t *service;
};

static int flowie_cluster_raft_discard_message(
    void *ctx, const tr_raft_message_t *message) {
  (void)ctx;
  return message ? TURBO_OK : TURBO_EINVAL;
}

static int flowie_cluster_raft_store_snapshot(
    void *ctx, tr_raft_index_t index, tr_raft_term_t term,
    const tr_raft_conf_t *configuration, const uint8_t *data, size_t size) {
  flowie_cluster_raft_store_t *store = (flowie_cluster_raft_store_t *)ctx;
  if (!store) return TURBO_EINVAL;
  return tr_raft_wal_storage_store_snapshot(
      store->storage, index, term, configuration, data, size);
}

static int flowie_cluster_raft_config_valid(
    const flowie_cluster_raft_store_config_t *config) {
  return config && config->path && config->path[0] != '\0' &&
         config->self_id != 0u && config->voters && config->voter_count != 0u &&
         config->heartbeat_ticks != 0u && config->election_min_ticks != 0u &&
         config->election_max_ticks >= config->election_min_ticks &&
         config->initial_election_timeout_ticks >= config->election_min_ticks &&
         config->initial_election_timeout_ticks <= config->election_max_ticks &&
         config->max_log_entries != 0u && config->max_segments != 0u &&
         config->state_machine.apply_batch &&
         ((config->snapshot_threshold == 0u && !config->snapshot_create) ||
          (config->snapshot_threshold != 0u && config->snapshot_create &&
           config->max_snapshot_bytes != 0u));
}

int flowie_cluster_raft_store_open(
    const flowie_cluster_raft_store_config_t *config,
    flowie_cluster_raft_store_t **out) {
  flowie_cluster_raft_store_t *store;
  tr_raft_wal_storage_config_t storage_config;
  tr_raft_wal_recovery_t recovery;
  tr_raft_service_config_t service_config;
  int rc;
  if (out) *out = NULL;
  if (!out || !flowie_cluster_raft_config_valid(config)) return TURBO_EINVAL;
  store = (flowie_cluster_raft_store_t *)calloc(1u, sizeof(*store));
  if (!store) return TURBO_ENOMEM;
  memset(&storage_config, 0, sizeof(storage_config));
  storage_config.path_prefix = config->path;
  storage_config.segment_bytes = config->segment_bytes;
  storage_config.max_transaction_bytes = config->max_transaction_bytes;
  storage_config.max_segments = config->max_segments;
  storage_config.max_log_entries = config->max_log_entries;
  storage_config.create_if_missing = true;
  storage_config.max_snapshot_bytes = config->max_snapshot_bytes;
  rc = tr_raft_wal_storage_open(&storage_config, &store->storage);
  if (rc != TURBO_OK) goto fail;
  memset(&recovery, 0, sizeof(recovery));
  rc = tr_raft_wal_storage_load(store->storage, &recovery);
  if (rc != TURBO_OK) goto fail;
  if (recovery.snapshot_size != 0u) {
    if (!config->snapshot_restore) {
      rc = TURBO_EPROTO;
      goto recovery_fail;
    }
    rc = config->snapshot_restore(
        config->snapshot_restore_ctx, recovery.snapshot_index,
        recovery.snapshot_term, recovery.snapshot_data, recovery.snapshot_size);
    if (rc != TURBO_OK) goto recovery_fail;
  }
  memset(&service_config, 0, sizeof(service_config));
  service_config.core.self_id = config->self_id;
  service_config.core.voters = config->voters;
  service_config.core.voter_count = config->voter_count;
  service_config.core.initial_configuration =
      recovery.has_snapshot_configuration ? &recovery.snapshot_configuration : NULL;
  service_config.core.heartbeat_ticks = config->heartbeat_ticks;
  service_config.core.election_min_ticks = config->election_min_ticks;
  service_config.core.election_max_ticks = config->election_max_ticks;
  service_config.core.initial_election_timeout_ticks =
      config->initial_election_timeout_ticks;
  service_config.core.initial_term = recovery.term;
  service_config.core.initial_vote = recovery.voted_for;
  service_config.core.initial_last_log_index = recovery.snapshot_index;
  service_config.core.initial_last_log_term = recovery.snapshot_term;
  service_config.core.initial_log_entries =
      recovery.entry_count == 0u ? NULL : recovery.entries;
  service_config.core.initial_log_entry_count = recovery.entry_count;
  service_config.core.initial_commit_index = recovery.commit_index;
  service_config.core.initial_applied_index = recovery.snapshot_index;
  service_config.core.max_log_entries = config->max_log_entries;
  service_config.core.max_inflight_append_requests =
      config->max_inflight_append_requests;
  rc = tr_raft_wal_storage_bind(store->storage, &service_config.storage);
  if (rc != TURBO_OK) goto recovery_fail;
  service_config.transport = config->transport;
  if (!service_config.transport.enqueue)
    service_config.transport.enqueue = flowie_cluster_raft_discard_message;
  service_config.state_machine = config->state_machine;
  if (config->snapshot_threshold != 0u) {
    service_config.snapshot_policy.applied_entry_threshold =
        config->snapshot_threshold;
    service_config.snapshot_policy.max_snapshot_bytes =
        config->max_snapshot_bytes;
    service_config.snapshot_policy.create = config->snapshot_create;
    service_config.snapshot_policy.create_context = config->snapshot_create_ctx;
    service_config.snapshot_policy.store = flowie_cluster_raft_store_snapshot;
    service_config.snapshot_policy.store_context = store;
  }
  rc = tr_raft_service_create(&service_config, &store->service);
  if (rc == TURBO_OK) rc = tr_raft_service_poll(store->service);
recovery_fail:
  tr_raft_wal_recovery_destroy(&recovery);
  if (rc != TURBO_OK) goto fail;
  *out = store;
  return TURBO_OK;
fail:
  if (store->service) tr_raft_service_destroy(store->service);
  if (store->storage) (void)tr_raft_wal_storage_close(store->storage);
  free(store);
  return rc;
}

int flowie_cluster_raft_store_tick(flowie_cluster_raft_store_t *store,
                                   uint32_t elapsed_ticks,
                                   uint32_t next_election_timeout_ticks) {
  tr_raft_tick_t tick;
  if (!store || elapsed_ticks == 0u || next_election_timeout_ticks == 0u)
    return TURBO_EINVAL;
  tick.elapsed_ticks = elapsed_ticks;
  tick.next_election_timeout_ticks = next_election_timeout_ticks;
  return tr_raft_service_tick(store->service, &tick);
}

int flowie_cluster_raft_store_step(flowie_cluster_raft_store_t *store,
                                   const tr_raft_message_t *message) {
  if (!store || !message) return TURBO_EINVAL;
  return tr_raft_service_step(store->service, message);
}

int flowie_cluster_raft_store_propose(
    flowie_cluster_raft_store_t *store, const tr_raft_proposal_t *proposal,
    tr_raft_operation_status_t *out_receipt) {
  if (!store || !proposal || !out_receipt) return TURBO_EINVAL;
  return tr_raft_service_propose_with_receipt(store->service, proposal,
                                              out_receipt);
}

int flowie_cluster_raft_store_status(
    const flowie_cluster_raft_store_t *store,
    tr_raft_service_status_t *out_status) {
  if (!store || !out_status) return TURBO_EINVAL;
  return tr_raft_service_status(store->service, out_status);
}

int flowie_cluster_raft_store_configuration(
    const flowie_cluster_raft_store_t *store,
    tr_raft_conf_t *out_configuration) {
  if (!store || !out_configuration) return TURBO_EINVAL;
  return tr_raft_service_configuration(store->service, out_configuration);
}

int flowie_cluster_raft_store_close(flowie_cluster_raft_store_t *store) {
  int rc;
  if (!store) return TURBO_EINVAL;
  tr_raft_service_destroy(store->service);
  rc = tr_raft_wal_storage_close(store->storage);
  free(store);
  return rc;
}
