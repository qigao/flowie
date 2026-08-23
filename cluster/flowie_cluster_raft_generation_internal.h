#ifndef FLOWIE_CLUSTER_RAFT_GENERATION_INTERNAL_H
#define FLOWIE_CLUSTER_RAFT_GENERATION_INTERNAL_H

#include "flowie_cluster_publish_router_internal.h"
#include "flowie_cluster_raft_runtime_internal.h"
#include "flowie_cluster_state_machine_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_cluster_raft_generation_s
    flowie_cluster_raft_generation_t;

typedef struct flowie_cluster_raft_publish_request_s {
  uint64_t stream_id;
  uint64_t command_id;
  flowie_mqtt_version_t mqtt_version;
  flowie_protocol_settlement_point_t requested_settlement;
  uint64_t connection_id;
  uint64_t connection_generation;
  uint64_t session_id;
  uint64_t session_generation;
  uint64_t accepted_at_epoch_seconds;
  vstr edge_node_id;
  const uint8_t *edge_boot_id;
  flowie_mqtt_span_t client_id;
  flowie_mqtt_span_t packet;
} flowie_cluster_raft_publish_request_t;

typedef struct flowie_cluster_raft_generation_config_s {
  flowie_cluster_owner_directory_config_t owners;
  flowie_cluster_publish_descriptor_apply_fn apply_publish;
  void *publish_ctx;
  flowie_cluster_raft_runtime_config_t runtime;
  flowie_cluster_publish_router_config_t router;
} flowie_cluster_raft_generation_config_t;

typedef struct flowie_cluster_raft_generation_api_s {
  int (*owners_create)(const flowie_cluster_owner_directory_config_t *config,
                       flowie_cluster_owner_directory_t **out);
  void (*owners_destroy)(flowie_cluster_owner_directory_t *directory);
  int (*runtime_create)(const flowie_cluster_raft_runtime_config_t *config,
                        flowie_cluster_raft_runtime_t **out);
  int (*runtime_start)(flowie_cluster_raft_runtime_t *runtime);
  int (*runtime_drive)(flowie_cluster_raft_runtime_t *runtime,
                       uint32_t elapsed_ticks,
                       uint32_t next_election_timeout_ticks,
                       tr_raft_flowmq_peer_service_step_result_t *out_step);
  int (*runtime_propose)(flowie_cluster_raft_runtime_t *runtime,
                         const tr_raft_proposal_t *proposal,
                         tr_raft_operation_status_t *out_receipt);
  int (*runtime_status)(const flowie_cluster_raft_runtime_t *runtime,
                        flowie_cluster_raft_runtime_status_t *out_status);
  int (*runtime_configuration)(const flowie_cluster_raft_runtime_t *runtime,
                               tr_raft_conf_t *out_configuration);
  int (*runtime_stop)(flowie_cluster_raft_runtime_t *runtime);
  int (*runtime_destroy)(flowie_cluster_raft_runtime_t *runtime);
  int (*router_create_bound)(
      const flowie_cluster_publish_router_config_t *config,
      flowie_cluster_raft_runtime_t *runtime,
      flowie_cluster_publish_router_t **out);
  int (*router_retry)(flowie_cluster_publish_router_t *router);
  int (*router_submit_durable)(flowie_cluster_publish_router_t *router,
                               tr_raft_term_t term, uint64_t stream_id,
                               uint64_t command_id,
                               const tr_raft_conf_t *configuration,
                               tstr *event);
  int (*router_destroy)(flowie_cluster_publish_router_t *router);
} flowie_cluster_raft_generation_api_t;

int flowie_cluster_raft_generation_create(
    const flowie_cluster_raft_generation_config_t *config,
    flowie_cluster_raft_generation_t **out);
int flowie_cluster_raft_generation_create_with_api(
    const flowie_cluster_raft_generation_config_t *config,
    const flowie_cluster_raft_generation_api_t *api,
    flowie_cluster_raft_generation_t **out);
int flowie_cluster_raft_generation_start(
    flowie_cluster_raft_generation_t *generation);
int flowie_cluster_raft_generation_drive(
    flowie_cluster_raft_generation_t *generation, uint32_t elapsed_ticks,
    uint32_t next_election_timeout_ticks,
    tr_raft_flowmq_peer_service_step_result_t *out_step);
int flowie_cluster_raft_generation_propose_owner(
    flowie_cluster_raft_generation_t *generation, uint64_t command_id,
    const flowie_cluster_owner_command_t *command,
    tr_raft_operation_status_t *out_receipt);
/** Takes *event only after all validation succeeds. */
int flowie_cluster_raft_generation_submit_publish_durable(
    flowie_cluster_raft_generation_t *generation, uint64_t stream_id,
    uint64_t command_id, tstr *event);
int flowie_cluster_raft_generation_publish(
    flowie_cluster_raft_generation_t *generation,
    const flowie_cluster_raft_publish_request_t *request);
int flowie_cluster_raft_generation_stop(
    flowie_cluster_raft_generation_t *generation);
flowie_cluster_owner_directory_t *flowie_cluster_raft_generation_owners(
    flowie_cluster_raft_generation_t *generation);
int flowie_cluster_raft_generation_destroy(
    flowie_cluster_raft_generation_t *generation);

#ifdef __cplusplus
}
#endif

#endif
