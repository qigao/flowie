#ifndef FLOWIE_CLUSTER_RAFT_RUNTIME_INTERNAL_H
#define FLOWIE_CLUSTER_RAFT_RUNTIME_INTERNAL_H

#include "flowie_cluster_raft_store_internal.h"

#include <turboraft/raft_flowmq_peer_service.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_cluster_raft_runtime_s flowie_cluster_raft_runtime_t;

typedef int (*flowie_cluster_raft_payload_fn)(
    void *ctx, const tr_raft_transport_payload_t *payload);

typedef struct flowie_cluster_raft_runtime_config_s {
  flowie_cluster_raft_store_config_t store;
  tr_raft_flowmq_peer_service_config_t peers;
  flowie_cluster_raft_payload_fn on_payload;
  void *payload_ctx;
} flowie_cluster_raft_runtime_config_t;

typedef struct flowie_cluster_raft_runtime_status_s {
  tr_raft_service_status_t raft;
  tr_raft_flowmq_peer_service_status_t peers;
  int started;
} flowie_cluster_raft_runtime_status_t;

int flowie_cluster_raft_runtime_create(
    const flowie_cluster_raft_runtime_config_t *config,
    flowie_cluster_raft_runtime_t **out);
int flowie_cluster_raft_runtime_bind_payload_handler(
    flowie_cluster_raft_runtime_t *runtime,
    flowie_cluster_raft_payload_fn on_payload, void *payload_ctx);
int flowie_cluster_raft_runtime_unbind_payload_handler(
    flowie_cluster_raft_runtime_t *runtime, void *expected_payload_ctx);
int flowie_cluster_raft_runtime_enqueue_adapter(
    void *runtime, const tr_raft_transport_payload_t *payload);
int flowie_cluster_raft_runtime_propose_adapter(
    void *runtime, const tr_raft_proposal_t *proposal);
int flowie_cluster_raft_runtime_start(flowie_cluster_raft_runtime_t *runtime);
int flowie_cluster_raft_runtime_drive(
    flowie_cluster_raft_runtime_t *runtime, uint32_t elapsed_ticks,
    uint32_t next_election_timeout_ticks,
    tr_raft_flowmq_peer_service_step_result_t *out_step);
int flowie_cluster_raft_runtime_enqueue_payload(
    flowie_cluster_raft_runtime_t *runtime,
    const tr_raft_transport_payload_t *payload);
int flowie_cluster_raft_runtime_propose(
    flowie_cluster_raft_runtime_t *runtime,
    const tr_raft_proposal_t *proposal,
    tr_raft_operation_status_t *out_receipt);
int flowie_cluster_raft_runtime_status(
    const flowie_cluster_raft_runtime_t *runtime,
    flowie_cluster_raft_runtime_status_t *out_status);
int flowie_cluster_raft_runtime_configuration(
    const flowie_cluster_raft_runtime_t *runtime,
    tr_raft_conf_t *out_configuration);
int flowie_cluster_raft_runtime_stop(flowie_cluster_raft_runtime_t *runtime);
int flowie_cluster_raft_runtime_destroy(flowie_cluster_raft_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
