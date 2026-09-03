#include "flowie_cluster_raft_runtime_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

static int flowie_raft_runtime_apply(void *ctx,
                                     const tr_raft_entry_t *entries,
                                     size_t entry_count) {
  (void)ctx;
  return entries && entry_count != 0u ? SALTS_OK : SALTS_EINVAL;
}

static int flowie_raft_runtime_payload(
    void *ctx, const tr_raft_transport_payload_t *payload) {
  (void)ctx;
  return payload ? SALTS_OK : SALTS_EINVAL;
}

spec("flowie cluster TurboRaft FlowMQ runtime") {
  it("rejects a voter topology without an exact FlowMQ peer mapping") {
    static const tr_raft_node_id_t voters[] = {1u, 2u};
    flowie_cluster_raft_runtime_config_t config;
    flowie_cluster_raft_runtime_t *runtime = NULL;
    memset(&config, 0, sizeof(config));
    config.store.path = ":memory:";
    config.store.self_id = 1u;
    config.store.voters = voters;
    config.store.voter_count = 2u;
    config.store.heartbeat_ticks = 1u;
    config.store.election_min_ticks = 3u;
    config.store.election_max_ticks = 5u;
    config.store.initial_election_timeout_ticks = 3u;
    config.store.max_log_entries = 16u;
    config.store.max_segments = 4u;
    config.store.state_machine.apply_batch = flowie_raft_runtime_apply;
    config.on_payload = flowie_raft_runtime_payload;
    check_equal(flowie_cluster_raft_runtime_create(&config, &runtime),
                 SALTS_EINVAL);
    check_null(runtime);
  }
}
