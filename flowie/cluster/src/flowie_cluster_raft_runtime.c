#include "flowie_cluster_raft_runtime_internal.h"

#include <core/error.h>

#include <stdlib.h>
#include <string.h>

#define FLOWIE_CLUSTER_RAFT_PRIMARY_GROUP_ID ((tr_raft_group_id_t)1u)

typedef enum flowie_cluster_raft_runtime_state_e {
  FLOWIE_RAFT_CREATED,
  FLOWIE_RAFT_RUNNING,
  FLOWIE_RAFT_STOPPING
} flowie_cluster_raft_runtime_state_t;

typedef struct flowie_cluster_raft_peer_s {
  tr_raft_node_id_t node_id;
  tr_raft_cnet_peer_t *link;
} flowie_cluster_raft_peer_t;

struct flowie_cluster_raft_runtime_s {
  flowie_cluster_raft_store_t *store;
  flowie_cluster_raft_peer_t peers[TR_RAFT_MAX_VOTERS - 1u];
  size_t peer_count;
  flowie_cluster_raft_payload_fn on_payload;
  void *payload_ctx;
  tr_raft_group_id_t group_id;
  flowie_cluster_raft_runtime_state_t state;
};

static tr_raft_cnet_peer_t *
flowie_cluster_raft_find_peer(const flowie_cluster_raft_runtime_t *runtime,
                              tr_raft_node_id_t node_id) {
  size_t index;
  for (index = 0u; index < runtime->peer_count; ++index)
    if (runtime->peers[index].node_id == node_id)
      return runtime->peers[index].link;
  return NULL;
}

static int flowie_cluster_raft_runtime_payload(void *ctx,
                                               const tr_raft_transport_payload_t *payload) {
  flowie_cluster_raft_runtime_t *runtime = (flowie_cluster_raft_runtime_t *)ctx;
  if (!runtime || !payload || payload->group_id != runtime->group_id)
    return SALTS_EPROTO;
  if (runtime->state != FLOWIE_RAFT_RUNNING)
    return SALTS_EPIPE;
  if (payload->kind == TR_RAFT_WIRE_PAYLOAD_RAFT)
    return runtime->store ? flowie_cluster_raft_store_step(runtime->store, &payload->data.raft)
                          : SALTS_EINVAL;
  if (!runtime->on_payload)
    return SALTS_EPROTO;
  return runtime->on_payload(runtime->payload_ctx, payload);
}

static int flowie_cluster_raft_runtime_message_enqueue(void *ctx,
                                                       const tr_raft_message_t *message) {
  flowie_cluster_raft_runtime_t *runtime = (flowie_cluster_raft_runtime_t *)ctx;
  tr_raft_cnet_peer_t *peer;
  if (!runtime || !message)
    return SALTS_EINVAL;
  peer = flowie_cluster_raft_find_peer(runtime, message->to);
  if (!peer)
    return SALTS_ENOENT;
  return tr_raft_cnet_peer_enqueue_group(peer, runtime->group_id, message);
}

static int
flowie_cluster_raft_runtime_topology_valid(const flowie_cluster_raft_runtime_config_t *config) {
  size_t voter_index;
  size_t peer_index;
  if (!config || config->store.self_id == 0u || !config->store.voters ||
      config->store.voter_count == 0u || config->store.voter_count > TR_RAFT_MAX_VOTERS ||
      config->peers.peer_count != config->store.voter_count - 1u ||
      (config->peers.peer_count != 0u && !config->peers.peers))
    return 0;
  for (voter_index = 0u; voter_index < config->store.voter_count; ++voter_index) {
    tr_raft_node_id_t voter = config->store.voters[voter_index];
    size_t matches = voter == config->store.self_id ? 1u : 0u;
    if (voter == 0u)
      return 0;
    for (peer_index = 0u; peer_index < config->peers.peer_count; ++peer_index)
      if (config->peers.peers[peer_index].transport.peer_node_id == voter)
        ++matches;
    if (matches != 1u)
      return 0;
  }
  for (peer_index = 0u; peer_index < config->peers.peer_count; ++peer_index) {
    const tr_raft_cnet_peer_config_t *peer = &config->peers.peers[peer_index];
    if (peer->transport.local_node_id != config->store.self_id ||
        peer->transport.peer_node_id == config->store.self_id || peer->transport.on_payload ||
        peer->transport.payload_context)
      return 0;
    for (voter_index = 0u; voter_index < peer_index; ++voter_index)
      if (peer->client == config->peers.peers[voter_index].client ||
          memcmp(&peer->transport.cluster_id,
                 &config->peers.peers[voter_index].transport.cluster_id,
                 sizeof(peer->transport.cluster_id)) != 0)
        return 0;
  }
  return 1;
}

int flowie_cluster_raft_runtime_create(const flowie_cluster_raft_runtime_config_t *config,
                                       flowie_cluster_raft_runtime_t **out) {
  flowie_cluster_raft_runtime_t *runtime;
  flowie_cluster_raft_store_config_t store_config;
  size_t index;
  int rc;
  if (out)
    *out = NULL;
  if (!out || !flowie_cluster_raft_runtime_topology_valid(config))
    return SALTS_EINVAL;
  runtime = (flowie_cluster_raft_runtime_t *)calloc(1u, sizeof(*runtime));
  if (!runtime)
    return SALTS_ENOMEM;
  runtime->on_payload = config->on_payload;
  runtime->payload_ctx = config->payload_ctx;
  runtime->group_id = FLOWIE_CLUSTER_RAFT_PRIMARY_GROUP_ID;
  runtime->peer_count = config->peers.peer_count;
  for (index = 0u; index < runtime->peer_count; ++index) {
    tr_raft_cnet_peer_config_t peer_config = config->peers.peers[index];
    runtime->peers[index].node_id = peer_config.transport.peer_node_id;
    peer_config.transport.on_payload = flowie_cluster_raft_runtime_payload;
    peer_config.transport.payload_context = runtime;
    rc = tr_raft_cnet_peer_create(&peer_config, &runtime->peers[index].link);
    if (rc != SALTS_OK)
      goto fail;
  }
  store_config = config->store;
  store_config.transport.context = runtime;
  store_config.transport.enqueue = flowie_cluster_raft_runtime_message_enqueue;
  rc = flowie_cluster_raft_store_open(&store_config, &runtime->store);
  if (rc != SALTS_OK)
    goto fail;
  *out = runtime;
  return SALTS_OK;
fail:
  if (runtime->store)
    (void)flowie_cluster_raft_store_close(runtime->store);
  for (index = 0u; index < runtime->peer_count; ++index)
    (void)tr_raft_cnet_peer_destroy(runtime->peers[index].link);
  free(runtime);
  return rc;
}

int flowie_cluster_raft_runtime_peer_observer(flowie_cluster_raft_runtime_t *runtime,
                                              tr_raft_node_id_t peer_node_id,
                                              cnet_observer *out_observer) {
  tr_raft_cnet_peer_t *peer;
  if (!runtime || !out_observer || runtime->state != FLOWIE_RAFT_CREATED)
    return SALTS_EINVAL;
  peer = flowie_cluster_raft_find_peer(runtime, peer_node_id);
  if (!peer)
    return SALTS_ENOENT;
  *out_observer = tr_raft_cnet_peer_observer(peer);
  return SALTS_OK;
}

int flowie_cluster_raft_runtime_bind_payload_handler(flowie_cluster_raft_runtime_t *runtime,
                                                     flowie_cluster_raft_payload_fn on_payload,
                                                     void *payload_ctx) {
  if (!runtime || runtime->state != FLOWIE_RAFT_CREATED || !on_payload)
    return SALTS_EINVAL;
  if (runtime->on_payload &&
      (runtime->on_payload != on_payload || runtime->payload_ctx != payload_ctx))
    return SALTS_EBUSY;
  runtime->on_payload = on_payload;
  runtime->payload_ctx = payload_ctx;
  return SALTS_OK;
}

int flowie_cluster_raft_runtime_unbind_payload_handler(flowie_cluster_raft_runtime_t *runtime,
                                                       void *expected_payload_ctx) {
  if (!runtime || runtime->state == FLOWIE_RAFT_RUNNING || !runtime->on_payload ||
      runtime->payload_ctx != expected_payload_ctx)
    return SALTS_EINVAL;
  runtime->on_payload = NULL;
  runtime->payload_ctx = NULL;
  return SALTS_OK;
}

int flowie_cluster_raft_runtime_enqueue_adapter(void *runtime,
                                                const tr_raft_transport_payload_t *payload) {
  return flowie_cluster_raft_runtime_enqueue_payload((flowie_cluster_raft_runtime_t *)runtime,
                                                     payload);
}

int flowie_cluster_raft_runtime_propose_adapter(void *runtime, const tr_raft_proposal_t *proposal) {
  tr_raft_operation_status_t receipt;
  if (!runtime || !proposal)
    return SALTS_EINVAL;
  memset(&receipt, 0, sizeof(receipt));
  return flowie_cluster_raft_runtime_propose((flowie_cluster_raft_runtime_t *)runtime, proposal,
                                             &receipt);
}

int flowie_cluster_raft_runtime_start(flowie_cluster_raft_runtime_t *runtime) {
  if (!runtime || runtime->state != FLOWIE_RAFT_CREATED || !runtime->on_payload)
    return SALTS_EINVAL;
  runtime->state = FLOWIE_RAFT_RUNNING;
  return SALTS_OK;
}

int flowie_cluster_raft_runtime_drive(flowie_cluster_raft_runtime_t *runtime,
                                      uint32_t elapsed_ticks, uint32_t next_election_timeout_ticks,
                                      flowie_cluster_raft_peer_step_t *out_step) {
  size_t index;
  int rc;
  if (!runtime || runtime->state != FLOWIE_RAFT_RUNNING || !out_step || elapsed_ticks == 0u ||
      next_election_timeout_ticks == 0u)
    return SALTS_EINVAL;
  memset(out_step, 0, sizeof(*out_step));
  for (index = 0u; index < runtime->peer_count; ++index) {
    tr_raft_cnet_peer_status_t status;
    rc = tr_raft_cnet_peer_get_status(runtime->peers[index].link, &status);
    if (rc == SALTS_OK && (status.connection_state == CNET_CONNECTION_FAILED ||
                           status.transport.state == TR_RAFT_TRANSPORT_STATE_FAULTED))
      rc = status.last_error != SALTS_OK ? status.last_error : SALTS_EIO;
    if (rc == SALTS_OK) {
      rc = tr_raft_cnet_peer_step(runtime->peers[index].link);
      /* EBUSY is the SDK contract for an idle queue or an in-flight CNet write. */
      if (rc == SALTS_EBUSY)
        rc = SALTS_OK;
    }
    if (rc != SALTS_OK) {
      out_step->failed_peer_count = 1u;
      out_step->first_error = rc;
      return rc;
    }
    ++out_step->stepped_peer_count;
  }
  return flowie_cluster_raft_store_tick(runtime->store, elapsed_ticks, next_election_timeout_ticks);
}

int flowie_cluster_raft_runtime_enqueue_payload(flowie_cluster_raft_runtime_t *runtime,
                                                const tr_raft_transport_payload_t *payload) {
  tr_raft_transport_payload_t grouped;
  tr_raft_node_id_t destination;
  tr_raft_cnet_peer_t *peer;
  if (!runtime || runtime->state != FLOWIE_RAFT_RUNNING || !payload)
    return SALTS_EINVAL;
  if (payload->group_id != 0u && payload->group_id != runtime->group_id)
    return SALTS_EINVAL;
  grouped = *payload;
  grouped.group_id = runtime->group_id;
  switch (payload->kind) {
  case TR_RAFT_WIRE_PAYLOAD_RAFT:
    destination = payload->data.raft.to;
    break;
  case TR_RAFT_WIRE_PAYLOAD_SNAPSHOT_CHUNK:
    destination = payload->data.snapshot_chunk.to;
    break;
  case TR_RAFT_WIRE_PAYLOAD_SNAPSHOT_ACK:
    destination = payload->data.snapshot_ack.to;
    break;
  case TR_RAFT_WIRE_PAYLOAD_DATA_CHUNK:
    destination = payload->data.data_chunk.to;
    break;
  case TR_RAFT_WIRE_PAYLOAD_DATA_ACK:
    destination = payload->data.data_ack.to;
    break;
  default:
    return SALTS_EINVAL;
  }
  peer = flowie_cluster_raft_find_peer(runtime, destination);
  if (!peer)
    return SALTS_ENOENT;
  return tr_raft_cnet_peer_enqueue_payload(peer, &grouped);
}

int flowie_cluster_raft_runtime_propose(flowie_cluster_raft_runtime_t *runtime,
                                        const tr_raft_proposal_t *proposal,
                                        tr_raft_operation_status_t *out_receipt) {
  if (!runtime || runtime->state != FLOWIE_RAFT_RUNNING)
    return SALTS_EINVAL;
  return flowie_cluster_raft_store_propose(runtime->store, proposal, out_receipt);
}

int flowie_cluster_raft_runtime_status(const flowie_cluster_raft_runtime_t *runtime,
                                       flowie_cluster_raft_runtime_status_t *out_status) {
  int rc;
  size_t index;
  if (!runtime || !out_status)
    return SALTS_EINVAL;
  memset(out_status, 0, sizeof(*out_status));
  rc = flowie_cluster_raft_store_status(runtime->store, &out_status->raft);
  if (rc != SALTS_OK)
    return rc;
  out_status->peers.peer_count = runtime->peer_count;
  for (index = 0u; index < runtime->peer_count; ++index) {
    tr_raft_cnet_peer_status_t peer;
    if (!runtime->peers[index].link)
      continue;
    rc = tr_raft_cnet_peer_get_status(runtime->peers[index].link, &peer);
    if (rc != SALTS_OK)
      return rc;
    if (peer.queued_payload_count > SIZE_MAX - out_status->peers.queued_payload_count ||
        peer.queued_data_bytes > SIZE_MAX - out_status->peers.queued_data_bytes)
      return SALTS_ERANGE;
    out_status->peers.connected_peer_count += peer.connection_state == CNET_CONNECTION_CONNECTED;
    out_status->peers.queued_payload_count += peer.queued_payload_count;
    out_status->peers.queued_data_bytes += peer.queued_data_bytes;
  }
  out_status->started = runtime->state == FLOWIE_RAFT_RUNNING;
  return SALTS_OK;
}

int flowie_cluster_raft_runtime_configuration(const flowie_cluster_raft_runtime_t *runtime,
                                              tr_raft_conf_t *out_configuration) {
  if (!runtime || runtime->state != FLOWIE_RAFT_RUNNING || !out_configuration)
    return SALTS_EINVAL;
  return flowie_cluster_raft_store_configuration(runtime->store, out_configuration);
}

int flowie_cluster_raft_runtime_stop(flowie_cluster_raft_runtime_t *runtime) {
  size_t index;
  int rc = SALTS_OK;
  if (!runtime)
    return SALTS_EINVAL;
  runtime->state = FLOWIE_RAFT_STOPPING;
  for (index = 0u; index < runtime->peer_count; ++index) {
    if (!runtime->peers[index].link)
      continue;
    int close_rc = tr_raft_cnet_peer_stop(runtime->peers[index].link);
    if (rc == SALTS_OK)
      rc = close_rc;
  }
  return rc;
}

int flowie_cluster_raft_runtime_destroy(flowie_cluster_raft_runtime_t *runtime) {
  int rc;
  size_t index;
  if (!runtime || runtime->state == FLOWIE_RAFT_RUNNING)
    return SALTS_EINVAL;
  runtime->state = FLOWIE_RAFT_STOPPING;
  for (index = 0u; index < runtime->peer_count; ++index) {
    rc = tr_raft_cnet_peer_destroy(runtime->peers[index].link);
    if (rc != SALTS_OK)
      return rc;
    runtime->peers[index].link = NULL;
  }
  rc = flowie_cluster_raft_store_close(runtime->store);
  free(runtime);
  return rc;
}
