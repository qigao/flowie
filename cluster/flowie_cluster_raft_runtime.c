#include "flowie_cluster_raft_runtime_internal.h"

#include "salts_error.h"

#include <stdlib.h>
#include <string.h>

struct flowie_cluster_raft_runtime_s {
  flowie_cluster_raft_store_t *store;
  tr_raft_flowmq_peer_service_t *peers;
  flowie_cluster_raft_payload_fn on_payload;
  void *payload_ctx;
  int started;
};

static int flowie_cluster_raft_runtime_message(
    void *ctx, const tr_raft_message_t *message) {
  flowie_cluster_raft_runtime_t *runtime =
      (flowie_cluster_raft_runtime_t *)ctx;
  return runtime && runtime->store
             ? flowie_cluster_raft_store_step(runtime->store, message)
             : SALTS_EINVAL;
}

static int flowie_cluster_raft_runtime_payload(
    void *ctx, const tr_raft_transport_payload_t *payload) {
  flowie_cluster_raft_runtime_t *runtime =
      (flowie_cluster_raft_runtime_t *)ctx;
  if (!runtime || !payload || !runtime->on_payload) return SALTS_EPROTO;
  return runtime->on_payload(runtime->payload_ctx, payload);
}

static int flowie_cluster_raft_runtime_topology_valid(
    const flowie_cluster_raft_runtime_config_t *config) {
  size_t voter_index;
  size_t peer_index;
  if (!config || config->store.self_id == 0u || !config->store.voters ||
      config->store.voter_count == 0u ||
      config->peers.peer_count + 1u != config->store.voter_count ||
      (config->peers.peer_count != 0u && !config->peers.peers) ||
      config->peers.on_message || config->peers.message_context ||
      config->peers.on_payload || config->peers.payload_context)
    return 0;
  for (voter_index = 0u; voter_index < config->store.voter_count;
       ++voter_index) {
    tr_raft_node_id_t voter = config->store.voters[voter_index];
    size_t matches = voter == config->store.self_id ? 1u : 0u;
    if (voter == 0u) return 0;
    for (peer_index = 0u; peer_index < config->peers.peer_count; ++peer_index)
      if (config->peers.peers[peer_index].node_id == voter) ++matches;
    if (matches != 1u) return 0;
  }
  for (peer_index = 0u; peer_index < config->peers.peer_count; ++peer_index)
    if (config->peers.peers[peer_index].node_id == config->store.self_id)
      return 0;
  return 1;
}

int flowie_cluster_raft_runtime_create(
    const flowie_cluster_raft_runtime_config_t *config,
    flowie_cluster_raft_runtime_t **out) {
  flowie_cluster_raft_runtime_t *runtime;
  tr_raft_flowmq_peer_service_config_t peer_config;
  flowie_cluster_raft_store_config_t store_config;
  int rc;
  if (out) *out = NULL;
  if (!out || !flowie_cluster_raft_runtime_topology_valid(config))
    return SALTS_EINVAL;
  runtime = (flowie_cluster_raft_runtime_t *)calloc(1u, sizeof(*runtime));
  if (!runtime) return SALTS_ENOMEM;
  runtime->on_payload = config->on_payload;
  runtime->payload_ctx = config->payload_ctx;
  peer_config = config->peers;
  peer_config.on_message = flowie_cluster_raft_runtime_message;
  peer_config.message_context = runtime;
  peer_config.on_payload = flowie_cluster_raft_runtime_payload;
  peer_config.payload_context = runtime;
  rc = tr_raft_flowmq_peer_service_create(&peer_config, &runtime->peers);
  if (rc != SALTS_OK) goto fail;
  store_config = config->store;
  store_config.transport.context = runtime->peers;
  store_config.transport.enqueue = tr_raft_flowmq_peer_service_enqueue;
  rc = flowie_cluster_raft_store_open(&store_config, &runtime->store);
  if (rc != SALTS_OK) goto fail;
  *out = runtime;
  return SALTS_OK;
fail:
  if (runtime->store) (void)flowie_cluster_raft_store_close(runtime->store);
  if (runtime->peers) (void)tr_raft_flowmq_peer_service_destroy(runtime->peers);
  free(runtime);
  return rc;
}

int flowie_cluster_raft_runtime_bind_payload_handler(
    flowie_cluster_raft_runtime_t *runtime,
    flowie_cluster_raft_payload_fn on_payload, void *payload_ctx) {
  if (!runtime || runtime->started || !on_payload) return SALTS_EINVAL;
  if (runtime->on_payload &&
      (runtime->on_payload != on_payload || runtime->payload_ctx != payload_ctx))
    return SALTS_EBUSY;
  runtime->on_payload = on_payload;
  runtime->payload_ctx = payload_ctx;
  return SALTS_OK;
}

int flowie_cluster_raft_runtime_unbind_payload_handler(
    flowie_cluster_raft_runtime_t *runtime, void *expected_payload_ctx) {
  if (!runtime || runtime->started || !runtime->on_payload ||
      runtime->payload_ctx != expected_payload_ctx)
    return SALTS_EINVAL;
  runtime->on_payload = NULL;
  runtime->payload_ctx = NULL;
  return SALTS_OK;
}

int flowie_cluster_raft_runtime_enqueue_adapter(
    void *runtime, const tr_raft_transport_payload_t *payload) {
  return flowie_cluster_raft_runtime_enqueue_payload(
      (flowie_cluster_raft_runtime_t *)runtime, payload);
}

int flowie_cluster_raft_runtime_propose_adapter(
    void *runtime, const tr_raft_proposal_t *proposal) {
  tr_raft_operation_status_t receipt;
  if (!runtime || !proposal) return SALTS_EINVAL;
  memset(&receipt, 0, sizeof(receipt));
  return flowie_cluster_raft_runtime_propose(
      (flowie_cluster_raft_runtime_t *)runtime, proposal, &receipt);
}

int flowie_cluster_raft_runtime_start(flowie_cluster_raft_runtime_t *runtime) {
  int rc;
  if (!runtime || runtime->started || !runtime->on_payload)
    return SALTS_EINVAL;
  rc = tr_raft_flowmq_peer_service_start(runtime->peers);
  if (rc == SALTS_OK) runtime->started = 1;
  return rc;
}

int flowie_cluster_raft_runtime_drive(
    flowie_cluster_raft_runtime_t *runtime, uint32_t elapsed_ticks,
    uint32_t next_election_timeout_ticks,
    tr_raft_flowmq_peer_service_step_result_t *out_step) {
  int rc;
  if (!runtime || !runtime->started || !out_step || elapsed_ticks == 0u ||
      next_election_timeout_ticks == 0u)
    return SALTS_EINVAL;
  memset(out_step, 0, sizeof(*out_step));
  rc = tr_raft_flowmq_peer_service_step(runtime->peers, out_step);
  if (rc != SALTS_OK) return rc;
  if (out_step->failed_peer_count != 0u)
    return out_step->first_error == SALTS_OK ? SALTS_EIO
                                             : out_step->first_error;
  return flowie_cluster_raft_store_tick(runtime->store, elapsed_ticks,
                                        next_election_timeout_ticks);
}

int flowie_cluster_raft_runtime_enqueue_payload(
    flowie_cluster_raft_runtime_t *runtime,
    const tr_raft_transport_payload_t *payload) {
  if (!runtime || !runtime->started || !payload) return SALTS_EINVAL;
  return tr_raft_flowmq_peer_service_enqueue_payload(runtime->peers, payload);
}

int flowie_cluster_raft_runtime_propose(
    flowie_cluster_raft_runtime_t *runtime,
    const tr_raft_proposal_t *proposal,
    tr_raft_operation_status_t *out_receipt) {
  if (!runtime || !runtime->started) return SALTS_EINVAL;
  return flowie_cluster_raft_store_propose(runtime->store, proposal,
                                           out_receipt);
}

int flowie_cluster_raft_runtime_status(
    const flowie_cluster_raft_runtime_t *runtime,
    flowie_cluster_raft_runtime_status_t *out_status) {
  int rc;
  if (!runtime || !out_status) return SALTS_EINVAL;
  memset(out_status, 0, sizeof(*out_status));
  rc = flowie_cluster_raft_store_status(runtime->store, &out_status->raft);
  if (rc == SALTS_OK)
    rc = tr_raft_flowmq_peer_service_get_status(runtime->peers,
                                                &out_status->peers);
  if (rc == SALTS_OK) out_status->started = runtime->started;
  return rc;
}

int flowie_cluster_raft_runtime_configuration(
    const flowie_cluster_raft_runtime_t *runtime,
    tr_raft_conf_t *out_configuration) {
  if (!runtime || !runtime->started || !out_configuration)
    return SALTS_EINVAL;
  return flowie_cluster_raft_store_configuration(runtime->store,
                                                  out_configuration);
}

int flowie_cluster_raft_runtime_stop(flowie_cluster_raft_runtime_t *runtime) {
  int rc;
  if (!runtime || !runtime->started) return SALTS_EINVAL;
  rc = tr_raft_flowmq_peer_service_stop(runtime->peers);
  if (rc == SALTS_OK) runtime->started = 0;
  return rc;
}

int flowie_cluster_raft_runtime_destroy(flowie_cluster_raft_runtime_t *runtime) {
  int rc = SALTS_OK;
  int close_rc;
  if (!runtime || runtime->started) return SALTS_EINVAL;
  close_rc = flowie_cluster_raft_store_close(runtime->store);
  if (close_rc != SALTS_OK) rc = close_rc;
  close_rc = tr_raft_flowmq_peer_service_destroy(runtime->peers);
  if (rc == SALTS_OK && close_rc != SALTS_OK) rc = close_rc;
  free(runtime);
  return rc;
}
