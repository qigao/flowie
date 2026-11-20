#ifndef FLOWIE_CLUSTER_RAFT_RUNTIME_INTERNAL_H
#define FLOWIE_CLUSTER_RAFT_RUNTIME_INTERNAL_H

#include "flowie_cluster_raft_store_internal.h"

#include <turboraft/raft_cnet_peer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_cluster_raft_runtime_s flowie_cluster_raft_runtime_t;

typedef int (*flowie_cluster_raft_payload_fn)(void *ctx,
                                              const tr_raft_transport_payload_t *payload);

typedef struct flowie_cluster_raft_peers_config_s {
  const tr_raft_cnet_peer_config_t *peers;
  size_t peer_count;
} flowie_cluster_raft_peers_config_t;

typedef struct flowie_cluster_raft_peer_step_s {
  size_t stepped_peer_count;
  size_t failed_peer_count;
  int first_error;
} flowie_cluster_raft_peer_step_t;

typedef struct flowie_cluster_raft_peers_status_s {
  size_t peer_count;
  size_t connected_peer_count;
  size_t queued_payload_count;
  size_t queued_data_bytes;
} flowie_cluster_raft_peers_status_t;

/**
 * All operations and borrowed CNet clients share one progress owner.
 * Clients outlive destroy; completed handshake/config inputs are copied by create.
 * The host drives CNet polling and uses peer_observer for connect/accept.
 */
typedef struct flowie_cluster_raft_runtime_config_s {
  flowie_cluster_raft_store_config_t store;
  flowie_cluster_raft_peers_config_t peers;
  flowie_cluster_raft_payload_fn on_payload;
  void *payload_ctx;
} flowie_cluster_raft_runtime_config_t;

typedef struct flowie_cluster_raft_runtime_status_s {
  tr_raft_service_status_t raft;
  flowie_cluster_raft_peers_status_t peers;
  int started;
} flowie_cluster_raft_runtime_status_t;

int flowie_cluster_raft_runtime_create(const flowie_cluster_raft_runtime_config_t *config,
                                       flowie_cluster_raft_runtime_t **out);
/**
 * Supplies the configured peer's observer before start. It borrows the runtime
 * until successful destroy. Returns ENOENT for an unknown peer, EINVAL for
 * invalid output/state; out_observer is written only on success.
 */
int flowie_cluster_raft_runtime_peer_observer(flowie_cluster_raft_runtime_t *runtime,
                                              tr_raft_node_id_t peer_node_id,
                                              cnet_observer *out_observer);
int flowie_cluster_raft_runtime_bind_payload_handler(flowie_cluster_raft_runtime_t *runtime,
                                                     flowie_cluster_raft_payload_fn on_payload,
                                                     void *payload_ctx);
int flowie_cluster_raft_runtime_unbind_payload_handler(flowie_cluster_raft_runtime_t *runtime,
                                                       void *expected_payload_ctx);
int flowie_cluster_raft_runtime_enqueue_adapter(void *runtime,
                                                const tr_raft_transport_payload_t *payload);
int flowie_cluster_raft_runtime_propose_adapter(void *runtime, const tr_raft_proposal_t *proposal);
int flowie_cluster_raft_runtime_start(flowie_cluster_raft_runtime_t *runtime);
int flowie_cluster_raft_runtime_drive(flowie_cluster_raft_runtime_t *runtime,
                                      uint32_t elapsed_ticks, uint32_t next_election_timeout_ticks,
                                      flowie_cluster_raft_peer_step_t *out_step);
int flowie_cluster_raft_runtime_enqueue_payload(flowie_cluster_raft_runtime_t *runtime,
                                                const tr_raft_transport_payload_t *payload);
int flowie_cluster_raft_runtime_propose(flowie_cluster_raft_runtime_t *runtime,
                                        const tr_raft_proposal_t *proposal,
                                        tr_raft_operation_status_t *out_receipt);
int flowie_cluster_raft_runtime_status(const flowie_cluster_raft_runtime_t *runtime,
                                       flowie_cluster_raft_runtime_status_t *out_status);
int flowie_cluster_raft_runtime_configuration(const flowie_cluster_raft_runtime_t *runtime,
                                              tr_raft_conf_t *out_configuration);
/** Seal admission and begin close. The host polls clients through terminal callbacks. */
int flowie_cluster_raft_runtime_stop(flowie_cluster_raft_runtime_t *runtime);
/** EBUSY retains ownership; after peers quiesce, storage close consumes the runtime. */
int flowie_cluster_raft_runtime_destroy(flowie_cluster_raft_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
