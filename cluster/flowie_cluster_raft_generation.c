#include "flowie_cluster_raft_generation_internal.h"

#include "salts_error.h"

#include <stdlib.h>

typedef enum flowie_cluster_raft_generation_state_e {
  FLOWIE_CLUSTER_RAFT_GENERATION_CREATED = 0,
  FLOWIE_CLUSTER_RAFT_GENERATION_RUNNING,
  FLOWIE_CLUSTER_RAFT_GENERATION_STOPPED
} flowie_cluster_raft_generation_state_t;

struct flowie_cluster_raft_generation_s {
  const flowie_cluster_raft_generation_api_t *api;
  flowie_cluster_owner_directory_t *owners;
  flowie_cluster_state_machine_t state_machine;
  flowie_cluster_raft_runtime_t *runtime;
  flowie_cluster_publish_router_t *router;
  size_t max_event_bytes;
  flowie_cluster_raft_generation_state_t state;
};

static const flowie_cluster_raft_generation_api_t
    FLOWIE_CLUSTER_RAFT_GENERATION_DEFAULT_API = {
        flowie_cluster_owner_directory_create,
        flowie_cluster_owner_directory_destroy,
        flowie_cluster_raft_runtime_create,
        flowie_cluster_raft_runtime_start,
        flowie_cluster_raft_runtime_drive,
        flowie_cluster_raft_runtime_propose,
        flowie_cluster_raft_runtime_status,
        flowie_cluster_raft_runtime_configuration,
        flowie_cluster_raft_runtime_stop,
        flowie_cluster_raft_runtime_destroy,
        flowie_cluster_publish_router_create_bound,
        flowie_cluster_publish_router_retry,
        flowie_cluster_publish_router_submit_durable,
        flowie_cluster_publish_router_destroy};

static int flowie_cluster_raft_generation_api_valid(
    const flowie_cluster_raft_generation_api_t *api) {
  return api && api->owners_create && api->owners_destroy &&
         api->runtime_create && api->runtime_start &&
         api->runtime_drive && api->runtime_propose && api->runtime_stop &&
         api->runtime_status && api->runtime_configuration &&
         api->runtime_destroy &&
         api->router_create_bound && api->router_retry &&
         api->router_submit_durable && api->router_destroy;
}

static int flowie_cluster_raft_generation_config_valid(
    const flowie_cluster_raft_generation_config_t *config) {
  return config && config->apply_publish && !config->runtime.on_payload &&
         !config->runtime.payload_ctx &&
         !config->runtime.store.state_machine.context &&
         !config->runtime.store.state_machine.apply_batch &&
         !config->router.enqueue &&
         !config->router.enqueue_ctx && !config->router.propose &&
         !config->router.propose_ctx;
}

int flowie_cluster_raft_generation_create_with_api(
    const flowie_cluster_raft_generation_config_t *config,
    const flowie_cluster_raft_generation_api_t *api,
    flowie_cluster_raft_generation_t **out) {
  flowie_cluster_raft_generation_t *generation;
  flowie_cluster_raft_runtime_config_t runtime_config;
  int rc;
  if (out) *out = NULL;
  if (!out || !flowie_cluster_raft_generation_config_valid(config) ||
      !flowie_cluster_raft_generation_api_valid(api))
    return SALTS_EINVAL;
  generation =
      (flowie_cluster_raft_generation_t *)calloc(1u, sizeof(*generation));
  if (!generation) return SALTS_ENOMEM;
  generation->api = api;
  generation->max_event_bytes = config->router.max_event_bytes;
  rc = api->owners_create(&config->owners, &generation->owners);
  if (rc != SALTS_OK) goto fail;
  generation->state_machine.owners.directory = generation->owners;
  generation->state_machine.apply_publish = config->apply_publish;
  generation->state_machine.publish_ctx = config->publish_ctx;
  runtime_config = config->runtime;
  runtime_config.store.state_machine.context = &generation->state_machine;
  runtime_config.store.state_machine.apply_batch =
      flowie_cluster_state_machine_apply_batch;
  rc = api->runtime_create(&runtime_config, &generation->runtime);
  if (rc != SALTS_OK) goto fail;
  rc = api->router_create_bound(&config->router, generation->runtime,
                                &generation->router);
  if (rc != SALTS_OK) goto fail;
  generation->state = FLOWIE_CLUSTER_RAFT_GENERATION_CREATED;
  *out = generation;
  return SALTS_OK;

fail:
  if (generation->router) (void)api->router_destroy(generation->router);
  if (generation->runtime) (void)api->runtime_destroy(generation->runtime);
  if (generation->owners) api->owners_destroy(generation->owners);
  free(generation);
  return rc;
}

int flowie_cluster_raft_generation_create(
    const flowie_cluster_raft_generation_config_t *config,
    flowie_cluster_raft_generation_t **out) {
  return flowie_cluster_raft_generation_create_with_api(
      config, &FLOWIE_CLUSTER_RAFT_GENERATION_DEFAULT_API, out);
}

int flowie_cluster_raft_generation_start(
    flowie_cluster_raft_generation_t *generation) {
  int rc;
  if (!generation || generation->state != FLOWIE_CLUSTER_RAFT_GENERATION_CREATED)
    return SALTS_EINVAL;
  rc = generation->api->runtime_start(generation->runtime);
  if (rc == SALTS_OK)
    generation->state = FLOWIE_CLUSTER_RAFT_GENERATION_RUNNING;
  return rc;
}

int flowie_cluster_raft_generation_drive(
    flowie_cluster_raft_generation_t *generation, uint32_t elapsed_ticks,
    uint32_t next_election_timeout_ticks,
    tr_raft_flowmq_peer_service_step_result_t *out_step) {
  int rc;
  if (!generation ||
      generation->state != FLOWIE_CLUSTER_RAFT_GENERATION_RUNNING)
    return SALTS_EINVAL;
  rc = generation->api->runtime_drive(
      generation->runtime, elapsed_ticks, next_election_timeout_ticks,
      out_step);
  return rc == SALTS_OK ? generation->api->router_retry(generation->router)
                        : rc;
}

int flowie_cluster_raft_generation_propose_owner(
    flowie_cluster_raft_generation_t *generation, uint64_t command_id,
    const flowie_cluster_owner_command_t *command,
    tr_raft_operation_status_t *out_receipt) {
  uint8_t encoded[FLOWIE_CLUSTER_OWNER_COMMAND_MAX_SIZE];
  tr_raft_proposal_t proposal;
  size_t encoded_size;
  int rc;
  if (!generation ||
      generation->state != FLOWIE_CLUSTER_RAFT_GENERATION_RUNNING ||
      command_id == 0u || !command || !out_receipt)
    return SALTS_EINVAL;
  rc = flowie_cluster_owner_command_encode(command, encoded, sizeof(encoded),
                                           &encoded_size);
  if (rc != SALTS_OK) return rc;
  proposal.command_id = command_id;
  proposal.data = encoded;
  proposal.data_length = encoded_size;
  return generation->api->runtime_propose(generation->runtime, &proposal,
                                          out_receipt);
}

int flowie_cluster_raft_generation_submit_publish_durable(
    flowie_cluster_raft_generation_t *generation, uint64_t stream_id,
    uint64_t command_id, tstr *event) {
  flowie_cluster_raft_runtime_status_t status;
  tr_raft_conf_t configuration;
  int rc;
  if (!generation ||
      generation->state != FLOWIE_CLUSTER_RAFT_GENERATION_RUNNING ||
      stream_id == 0u || command_id == 0u || !event || !*event)
    return SALTS_EINVAL;
  rc = generation->api->runtime_status(generation->runtime, &status);
  if (rc != SALTS_OK) return rc;
  if (status.raft.core.role != TR_RAFT_LEADER || status.raft.core.term == 0u)
    return SALTS_EBUSY;
  rc = generation->api->runtime_configuration(generation->runtime,
                                               &configuration);
  if (rc != SALTS_OK) return rc;
  return generation->api->router_submit_durable(
      generation->router, status.raft.core.term, stream_id, command_id,
      &configuration, event);
}

int flowie_cluster_raft_generation_publish(
    flowie_cluster_raft_generation_t *generation,
    const flowie_cluster_raft_publish_request_t *request) {
  tstr event = NULL;
  int rc;
  if (!generation || !request || request->stream_id == 0u ||
      request->command_id == 0u || !request->edge_boot_id)
    return SALTS_EINVAL;
  rc = flowie_cluster_publish_event_encode(
      request->mqtt_version, request->requested_settlement,
      request->connection_id, request->connection_generation,
      request->session_id, request->session_generation,
      request->accepted_at_epoch_seconds, request->edge_node_id,
      request->edge_boot_id, request->client_id, request->packet,
      generation->max_event_bytes, &event);
  if (rc == SALTS_OK)
    rc = flowie_cluster_raft_generation_submit_publish_durable(
        generation, request->stream_id, request->command_id, &event);
  tstr_freep(&event);
  return rc;
}

int flowie_cluster_raft_generation_stop(
    flowie_cluster_raft_generation_t *generation) {
  int rc;
  if (!generation ||
      generation->state != FLOWIE_CLUSTER_RAFT_GENERATION_RUNNING)
    return SALTS_EINVAL;
  rc = generation->api->runtime_stop(generation->runtime);
  if (rc == SALTS_OK)
    generation->state = FLOWIE_CLUSTER_RAFT_GENERATION_STOPPED;
  return rc;
}

flowie_cluster_owner_directory_t *flowie_cluster_raft_generation_owners(
    flowie_cluster_raft_generation_t *generation) {
  return generation ? generation->owners : NULL;
}

int flowie_cluster_raft_generation_destroy(
    flowie_cluster_raft_generation_t *generation) {
  int rc;
  if (!generation) return SALTS_OK;
  if (generation->state == FLOWIE_CLUSTER_RAFT_GENERATION_RUNNING)
    return SALTS_EBUSY;
  rc = generation->api->router_destroy(generation->router);
  if (rc != SALTS_OK) return rc;
  generation->router = NULL;
  rc = generation->api->runtime_destroy(generation->runtime);
  if (rc != SALTS_OK) return rc;
  generation->runtime = NULL;
  generation->api->owners_destroy(generation->owners);
  generation->owners = NULL;
  free(generation);
  return SALTS_OK;
}
