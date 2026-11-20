#ifndef FLOWIE_CLUSTER_PUBLISH_INGRESS_INTERNAL_H
#define FLOWIE_CLUSTER_PUBLISH_INGRESS_INTERNAL_H

#include "flowie_cluster_publish_stream_internal.h"

#include <turboraft/raft_transport.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_cluster_publish_ingress_s flowie_cluster_publish_ingress_t;

typedef int (*flowie_cluster_publish_ingress_enqueue_fn)(
    void *ctx, const tr_raft_transport_payload_t *payload);

typedef struct flowie_cluster_publish_ingress_config_s {
  tr_raft_node_id_t self_id;
  size_t max_event_bytes;
  size_t max_active_streams;
  flowie_cluster_publish_stream_commit_fn commit;
  void *commit_ctx;
  flowie_cluster_publish_ingress_enqueue_fn enqueue;
  void *enqueue_ctx;
} flowie_cluster_publish_ingress_config_t;

int flowie_cluster_publish_ingress_create(
    const flowie_cluster_publish_ingress_config_t *config,
    flowie_cluster_publish_ingress_t **out);
void flowie_cluster_publish_ingress_destroy(
    flowie_cluster_publish_ingress_t *ingress);
int flowie_cluster_publish_ingress_handle(
    flowie_cluster_publish_ingress_t *ingress,
    const tr_raft_transport_payload_t *payload);
size_t flowie_cluster_publish_ingress_active_count(
    const flowie_cluster_publish_ingress_t *ingress);

#ifdef __cplusplus
}
#endif

#endif
