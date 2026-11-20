#include "flowie_cluster_raft_runtime_internal.h"

#include "tinytest.h"
#include <core/error.h>
#include <file/fs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_RAFT_TEST_SEGMENTS = 2,
  FLOWIE_RAFT_TEST_TRANSACTION_BYTES = 8 * 1024,
  FLOWIE_RAFT_TEST_LOG_ENTRIES = 16,
  FLOWIE_RAFT_TEST_QUEUE_ITEMS = 2,
  FLOWIE_RAFT_TEST_QUEUE_BYTES = 4096
};

static int flowie_raft_runtime_apply(void *ctx, const tr_raft_entry_t *entries,
                                     size_t entry_count) {
  (void)ctx;
  return entries && entry_count != 0u ? SALTS_OK : SALTS_EINVAL;
}

static int flowie_raft_runtime_payload(void *ctx, const tr_raft_transport_payload_t *payload) {
  (void)ctx;
  return payload ? SALTS_OK : SALTS_EINVAL;
}

static tr_raft_cnet_peer_config_t flowie_raft_runtime_peer(cnet_client *client,
                                                           tr_raft_handshake_result_t *handshake) {
  tr_raft_cnet_peer_config_t peer = {0};
  memset(handshake, 0, sizeof(*handshake));
  handshake->complete = 1;
  handshake->cluster_id.bytes[0] = 1u;
  handshake->local_node_id = 1u;
  handshake->peer_node_id = 2u;
  handshake->peer_process_incarnation.bytes[0] = 1u;
  handshake->wire_major = TR_RAFT_HANDSHAKE_WIRE_MAJOR;
  handshake->wire_minor = TR_RAFT_HANDSHAKE_WIRE_MINOR;
  handshake->max_frame_size = TR_RAFT_WIRE_MAX_FRAME_SIZE;
  handshake->max_snapshot_chunk_size = TR_RAFT_WIRE_MAX_SNAPSHOT_CHUNK_BYTES;
  peer.client = client;
  peer.transport.cluster_id = handshake->cluster_id;
  peer.transport.local_node_id = 1u;
  peer.transport.peer_node_id = 2u;
  peer.transport.first_outbound_message_id = 1u;
  peer.transport.handshake = handshake;
  peer.outbound_limits.total_item_capacity = FLOWIE_RAFT_TEST_QUEUE_ITEMS;
  peer.outbound_limits.total_data_bytes = FLOWIE_RAFT_TEST_QUEUE_BYTES;
  peer.outbound_limits.max_active_groups = 1u;
  peer.outbound_limits.per_group_item_capacity = FLOWIE_RAFT_TEST_QUEUE_ITEMS;
  peer.outbound_limits.per_group_data_bytes = FLOWIE_RAFT_TEST_QUEUE_BYTES;
  return peer;
}

static flowie_cluster_raft_runtime_config_t
flowie_raft_runtime_config(const char *path, const tr_raft_cnet_peer_config_t *peer) {
  static const tr_raft_node_id_t voters[] = {1u, 2u};
  flowie_cluster_raft_runtime_config_t config = {0};
  config.store.path = path;
  config.store.segment_bytes = TR_RAFT_WAL_MIN_SEGMENT_BYTES;
  config.store.max_transaction_bytes = FLOWIE_RAFT_TEST_TRANSACTION_BYTES;
  config.store.max_segments = FLOWIE_RAFT_TEST_SEGMENTS;
  config.store.self_id = 1u;
  config.store.voters = voters;
  config.store.voter_count = sizeof(voters) / sizeof(voters[0]);
  config.store.heartbeat_ticks = 1u;
  config.store.election_min_ticks = 3u;
  config.store.election_max_ticks = 5u;
  config.store.initial_election_timeout_ticks = 3u;
  config.store.max_log_entries = FLOWIE_RAFT_TEST_LOG_ENTRIES;
  config.store.max_inflight_append_requests = 1u;
  config.store.state_machine.apply_batch = flowie_raft_runtime_apply;
  config.peers.peers = peer;
  config.peers.peer_count = 1u;
  config.on_payload = flowie_raft_runtime_payload;
  return config;
}

static void flowie_raft_runtime_cleanup(const char *path) {
  char generated[SALTS_FS_MAX_PATH];
  size_t segment;
  if (!path)
    return;
  for (segment = 1u; segment <= FLOWIE_RAFT_TEST_SEGMENTS; ++segment) {
    snprintf(generated, sizeof(generated), "%s.%08zu.wal", path, segment);
    if (fs_access(generated, SALTS_FS_ACCESS_EXISTS) == SALTS_OK)
      check_equal(tt_remove_file(generated), 0);
  }
  snprintf(generated, sizeof(generated), "%s.lock", path);
  if (fs_access(generated, SALTS_FS_ACCESS_EXISTS) == SALTS_OK)
    check_equal(tt_remove_file(generated), 0);
  check_equal(tt_remove_file(path), 0);
}

spec("flowie server/cluster TurboRaft CNet runtime") {
  static flowie_cluster_raft_runtime_t *runtime;
  static char *path;
  static cnet_client client;
  static cnet_observer observer;
  static tr_raft_handshake_result_t handshake;
  static tr_raft_cnet_peer_config_t peer;
  static flowie_cluster_raft_runtime_config_t configured;

  before_each() {
    runtime = NULL;
    path = tt_make_temp_file("flowie-raft-runtime", ".data");
    check_not_null(path);
    memset(&client, 0, sizeof(client));
    memset(&observer, 0, sizeof(observer));
    peer = flowie_raft_runtime_peer(&client, &handshake);
    configured = flowie_raft_runtime_config(path, &peer);
  }

  after_each() {
    if (runtime) {
      if (observer.on_state) {
        cnet_connection connection = {0};
        observer.on_state(observer.user, connection, CNET_CONNECTION_CLOSED, NULL);
      }
      check_equal(flowie_cluster_raft_runtime_stop(runtime), SALTS_OK);
      check_equal(flowie_cluster_raft_runtime_destroy(runtime), SALTS_OK);
    }
    flowie_raft_runtime_cleanup(path);
    free(path);
  }

  it("rejects a voter topology without an exact CNet peer mapping") {
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
    check_equal(flowie_cluster_raft_runtime_create(&config, &runtime), SALTS_EINVAL);
    check_null(runtime);
  }

  it("routes to a bounded CNet peer queue without consuming rejected payloads") {
    flowie_cluster_raft_runtime_status_t status;
    flowie_cluster_raft_peer_step_t step;
    tr_raft_transport_payload_t payload = {0};
    check_equal(flowie_cluster_raft_runtime_create(&configured, &runtime), SALTS_OK);
    check_equal(flowie_cluster_raft_runtime_peer_observer(runtime, 2u, &observer), SALTS_OK);
    check_not_null(observer.on_receive);
    check_equal(flowie_cluster_raft_runtime_start(runtime), SALTS_OK);
    payload.kind = TR_RAFT_WIRE_PAYLOAD_RAFT;
    payload.data.raft.type = TR_RAFT_MSG_HEARTBEAT_REQUEST;
    payload.data.raft.from = 1u;
    payload.data.raft.to = 2u;
    payload.data.raft.term = 1u;
    check_equal(flowie_cluster_raft_runtime_enqueue_payload(runtime, &payload), SALTS_OK);
    check_equal(flowie_cluster_raft_runtime_enqueue_payload(runtime, &payload), SALTS_OK);
    check_equal(flowie_cluster_raft_runtime_enqueue_payload(runtime, &payload), SALTS_ENOSPC);
    check_equal(payload.group_id, 0u);
    payload.data.raft.to = 3u;
    check_equal(flowie_cluster_raft_runtime_enqueue_payload(runtime, &payload), SALTS_ENOENT);
    payload.group_id = 2u;
    check_equal(flowie_cluster_raft_runtime_enqueue_payload(runtime, &payload), SALTS_EINVAL);
    check_equal(flowie_cluster_raft_runtime_drive(runtime, 1u, 3u, &step), SALTS_OK);
    check_equal(step.stepped_peer_count, 1u);
    check_equal(flowie_cluster_raft_runtime_status(runtime, &status), SALTS_OK);
    check_equal(status.peers.queued_payload_count, FLOWIE_RAFT_TEST_QUEUE_ITEMS);
    check_equal(flowie_cluster_raft_runtime_stop(runtime), SALTS_OK);
    check_equal(flowie_cluster_raft_runtime_start(runtime), SALTS_EINVAL);
  }

  it("retains the runtime until borrowed CNet clients deliver terminal callbacks") {
    cnet_connection connection = {0};
    check_equal(flowie_cluster_raft_runtime_create(&configured, &runtime), SALTS_OK);
    check_equal(flowie_cluster_raft_runtime_peer_observer(runtime, 2u, &observer), SALTS_OK);
    observer.on_state(observer.user, connection, CNET_CONNECTION_CONNECTED, NULL);
    check_equal(flowie_cluster_raft_runtime_destroy(runtime), SALTS_EBUSY);
    check_equal(flowie_cluster_raft_runtime_start(runtime), SALTS_EINVAL);
    observer.on_state(observer.user, connection, CNET_CONNECTION_CLOSED, NULL);
    check_equal(flowie_cluster_raft_runtime_destroy(runtime), SALTS_OK);
    runtime = NULL;
  }

  it("propagates failed CNet connections before advancing the Raft clock") {
    cnet_connection connection = {0};
    cnet_error error = {0};
    flowie_cluster_raft_peer_step_t step;
    check_equal(flowie_cluster_raft_runtime_create(&configured, &runtime), SALTS_OK);
    check_equal(flowie_cluster_raft_runtime_peer_observer(runtime, 2u, &observer), SALTS_OK);
    check_equal(flowie_cluster_raft_runtime_start(runtime), SALTS_OK);
    error.status = SALTS_EIO;
    observer.on_state(observer.user, connection, CNET_CONNECTION_FAILED, &error);
    check_equal(flowie_cluster_raft_runtime_drive(runtime, 1u, 3u, &step), SALTS_EIO);
    check_equal(step.failed_peer_count, 1u);
    check_equal(step.first_error, SALTS_EIO);
  }
}
