#ifndef FLOWIE_CLUSTER_PUBLISH_ROUTER_INTERNAL_H
#define FLOWIE_CLUSTER_PUBLISH_ROUTER_INTERNAL_H

#include "flowie_cluster_publish_egress_internal.h"
#include "flowie_cluster_publish_ingress_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_cluster_publish_router_s flowie_cluster_publish_router_t;
typedef struct flowie_cluster_raft_runtime_s flowie_cluster_raft_runtime_t;

typedef int (*flowie_cluster_publish_router_propose_fn)(
    void *ctx, const tr_raft_proposal_t *proposal);

typedef struct flowie_cluster_publish_router_config_s {
  tr_raft_node_id_t self_id;
  size_t max_event_bytes;
  size_t max_inbound_streams;
  size_t max_outbound_streams;
  flowie_cluster_publish_stream_commit_fn commit;
  void *commit_ctx;
  flowie_cluster_publish_egress_enqueue_fn enqueue;
  void *enqueue_ctx;
  flowie_cluster_publish_router_propose_fn propose;
  void *propose_ctx;
} flowie_cluster_publish_router_config_t;

int flowie_cluster_publish_router_create(
    const flowie_cluster_publish_router_config_t *config,
    flowie_cluster_publish_router_t **out);
/** Binds runtime transport/proposal callbacks; those config fields must be unset. */
int flowie_cluster_publish_router_create_bound(
    const flowie_cluster_publish_router_config_t *config,
    flowie_cluster_raft_runtime_t *runtime,
    flowie_cluster_publish_router_t **out);
int flowie_cluster_publish_router_destroy(
    flowie_cluster_publish_router_t *router);

/** Takes *event after validation; the caller has already made local staging durable. */
int flowie_cluster_publish_router_submit_durable(
    flowie_cluster_publish_router_t *router, tr_raft_term_t term,
    uint64_t stream_id, uint64_t command_id,
    const tr_raft_conf_t *configuration, tstr *event);
int flowie_cluster_publish_router_handle(
    flowie_cluster_publish_router_t *router,
    const tr_raft_transport_payload_t *payload);
int flowie_cluster_publish_router_retry(
    flowie_cluster_publish_router_t *router);
size_t flowie_cluster_publish_router_outbound_count(
    const flowie_cluster_publish_router_t *router);

#ifdef __cplusplus
}
#endif

#endif
