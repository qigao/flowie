#include "flowie_stl_error_internal.h"

#include <cstl.h>
#include <cstl.h>
#include <cstl.h>
#include <cstl.h>

#include "flowie_cluster_publish_router_internal.h"

#include "flowie_cluster_raft_runtime_internal.h"

#include "salts_error.h"
#include <cstl.h>

#include <stdlib.h>
#include <string.h>

typedef struct flowie_cluster_publish_router_outbound_s {
  uint64_t stream_id;
  uint64_t command_id;
  flowie_cluster_publish_egress_t *egress;
} flowie_cluster_publish_router_outbound_t;

struct flowie_cluster_publish_router_s {
  flowie_cluster_publish_router_config_t config;
  flowie_cluster_publish_ingress_t *ingress;
  vec_t outbound;
  flowie_cluster_raft_runtime_t *bound_runtime;
};

static int flowie_cluster_publish_router_payload_adapter(
    void *ctx, const tr_raft_transport_payload_t *payload) {
  return flowie_cluster_publish_router_handle(
      (flowie_cluster_publish_router_t *)ctx, payload);
}

static flowie_cluster_publish_router_outbound_t *
flowie_cluster_publish_router_find(flowie_cluster_publish_router_t *router,
                                   uint64_t stream_id, size_t *out_index) {
  size_t index;
  for (index = 0u; index < vec_size(&router->outbound); ++index) {
    flowie_cluster_publish_router_outbound_t *entry =
        (flowie_cluster_publish_router_outbound_t *)vec_at(
            &router->outbound, index);
    if (entry->stream_id == stream_id) {
      if (out_index) *out_index = index;
      return entry;
    }
  }
  return NULL;
}

static void flowie_cluster_publish_router_remove(
    flowie_cluster_publish_router_t *router, size_t index) {
  flowie_cluster_publish_router_outbound_t removed;
  if (flowie_stl_error(vec_swap_remove(&router->outbound, index, &removed)) == SALTS_OK)
    flowie_cluster_publish_egress_destroy(removed.egress);
}

static int flowie_cluster_publish_router_try_propose(
    flowie_cluster_publish_router_t *router,
    flowie_cluster_publish_router_outbound_t *entry, size_t entry_index) {
  uint8_t descriptor[TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE];
  tr_raft_proposal_t proposal;
  int rc = flowie_cluster_publish_egress_make_proposal(
      entry->egress, entry->command_id, descriptor, &proposal);
  if (rc == SALTS_EBUSY) return SALTS_OK;
  if (rc != SALTS_OK) return rc;
  rc = router->config.propose(router->config.propose_ctx, &proposal);
  if (rc == SALTS_OK)
    flowie_cluster_publish_router_remove(router, entry_index);
  return rc;
}

int flowie_cluster_publish_router_create(
    const flowie_cluster_publish_router_config_t *config,
    flowie_cluster_publish_router_t **out) {
  flowie_cluster_publish_router_t *router;
  flowie_cluster_publish_ingress_config_t ingress_config;
  int rc;
  if (out) *out = NULL;
  if (!config || !out || config->self_id == 0u ||
      config->max_event_bytes == 0u || config->max_inbound_streams == 0u ||
      config->max_outbound_streams == 0u || !config->commit ||
      !config->enqueue || !config->propose)
    return SALTS_EINVAL;
  router = (flowie_cluster_publish_router_t *)calloc(1u, sizeof(*router));
  if (!router) return SALTS_ENOMEM;
  router->config = *config;
  rc = flowie_stl_error(vec_init_bytes(&router->outbound, sizeof(flowie_cluster_publish_router_outbound_t), _Alignof(flowie_cluster_publish_router_outbound_t), SIZE_MAX));
  if (rc == SALTS_OK)
    rc = flowie_stl_error(vec_reserve(&router->outbound,
                           config->max_outbound_streams));
  if (rc != SALTS_OK) goto fail;
  memset(&ingress_config, 0, sizeof(ingress_config));
  ingress_config.self_id = config->self_id;
  ingress_config.max_event_bytes = config->max_event_bytes;
  ingress_config.max_active_streams = config->max_inbound_streams;
  ingress_config.commit = config->commit;
  ingress_config.commit_ctx = config->commit_ctx;
  ingress_config.enqueue = config->enqueue;
  ingress_config.enqueue_ctx = config->enqueue_ctx;
  rc = flowie_cluster_publish_ingress_create(&ingress_config,
                                              &router->ingress);
  if (rc != SALTS_OK) goto fail;
  *out = router;
  return SALTS_OK;
fail:
  vec_destroy(&router->outbound);
  free(router);
  return rc;
}

int flowie_cluster_publish_router_create_bound(
    const flowie_cluster_publish_router_config_t *config,
    flowie_cluster_raft_runtime_t *runtime,
    flowie_cluster_publish_router_t **out) {
  flowie_cluster_publish_router_config_t bound_config;
  flowie_cluster_publish_router_t *router;
  int rc;
  if (out) *out = NULL;
  if (!config || !runtime || !out || config->enqueue || config->enqueue_ctx ||
      config->propose || config->propose_ctx)
    return SALTS_EINVAL;
  bound_config = *config;
  bound_config.enqueue = flowie_cluster_raft_runtime_enqueue_adapter;
  bound_config.enqueue_ctx = runtime;
  bound_config.propose = flowie_cluster_raft_runtime_propose_adapter;
  bound_config.propose_ctx = runtime;
  rc = flowie_cluster_publish_router_create(&bound_config, &router);
  if (rc != SALTS_OK) return rc;
  rc = flowie_cluster_raft_runtime_bind_payload_handler(
      runtime, flowie_cluster_publish_router_payload_adapter, router);
  if (rc != SALTS_OK) {
    (void)flowie_cluster_publish_router_destroy(router);
    return rc;
  }
  router->bound_runtime = runtime;
  *out = router;
  return SALTS_OK;
}

int flowie_cluster_publish_router_destroy(
    flowie_cluster_publish_router_t *router) {
  int rc;
  if (!router) return SALTS_EINVAL;
  if (router->bound_runtime) {
    rc = flowie_cluster_raft_runtime_unbind_payload_handler(
        router->bound_runtime, router);
    if (rc != SALTS_OK) return rc;
  }
  while (!vec_empty(&router->outbound))
    flowie_cluster_publish_router_remove(
        router, vec_size(&router->outbound) - 1u);
  flowie_cluster_publish_ingress_destroy(router->ingress);
  vec_destroy(&router->outbound);
  free(router);
  return SALTS_OK;
}

int flowie_cluster_publish_router_submit_durable(
    flowie_cluster_publish_router_t *router, tr_raft_term_t term,
    uint64_t stream_id, uint64_t command_id,
    const tr_raft_conf_t *configuration, tstr *event) {
  flowie_cluster_publish_egress_config_t egress_config;
  flowie_cluster_publish_router_outbound_t entry;
  size_t entry_index;
  int rc;
  if (!router || term == 0u || stream_id == 0u || command_id == 0u ||
      !configuration || !event || !*event)
    return SALTS_EINVAL;
  if (flowie_cluster_publish_router_find(router, stream_id, NULL))
    return SALTS_EBUSY;
  if (vec_size(&router->outbound) >=
      router->config.max_outbound_streams)
    return SALTS_ENOSPC;
  memset(&egress_config, 0, sizeof(egress_config));
  egress_config.self_id = router->config.self_id;
  egress_config.term = term;
  egress_config.stream_id = stream_id;
  egress_config.configuration = *configuration;
  egress_config.max_event_bytes = router->config.max_event_bytes;
  egress_config.enqueue = router->config.enqueue;
  egress_config.enqueue_ctx = router->config.enqueue_ctx;
  memset(&entry, 0, sizeof(entry));
  entry.stream_id = stream_id;
  entry.command_id = command_id;
  rc = flowie_cluster_publish_egress_create(&egress_config, event,
                                             &entry.egress);
  if (rc != SALTS_OK) return rc;
  rc = flowie_stl_error(vec_push(&router->outbound, &entry));
  if (rc != SALTS_OK) {
    flowie_cluster_publish_egress_destroy(entry.egress);
    return rc;
  }
  entry_index = vec_size(&router->outbound) - 1u;
  rc = flowie_cluster_publish_egress_mark_local_durable(entry.egress);
  if (rc == SALTS_OK) rc = flowie_cluster_publish_egress_pump(entry.egress);
  if (rc != SALTS_OK && rc != SALTS_ENOSPC && rc != SALTS_EBUSY) {
    flowie_cluster_publish_router_remove(router, entry_index);
    return rc;
  }
  return rc;
}

int flowie_cluster_publish_router_handle(
    flowie_cluster_publish_router_t *router,
    const tr_raft_transport_payload_t *payload) {
  flowie_cluster_publish_router_outbound_t *entry;
  size_t entry_index;
  int rc;
  if (!router || !payload) return SALTS_EINVAL;
  if (payload->kind == TR_RAFT_WIRE_PAYLOAD_DATA_CHUNK)
    return flowie_cluster_publish_ingress_handle(router->ingress, payload);
  if (payload->kind != TR_RAFT_WIRE_PAYLOAD_DATA_ACK) return SALTS_EINVAL;
  entry = flowie_cluster_publish_router_find(
      router, payload->data.data_ack.stream_id, &entry_index);
  if (!entry) return SALTS_ENOENT;
  rc = flowie_cluster_publish_egress_acknowledge(
      entry->egress, &payload->data.data_ack);
  return rc == SALTS_OK
             ? flowie_cluster_publish_router_try_propose(router, entry,
                                                         entry_index)
             : rc;
}

int flowie_cluster_publish_router_retry(
    flowie_cluster_publish_router_t *router) {
  size_t index = 0u;
  int rc;
  if (!router) return SALTS_EINVAL;
  while (index < vec_size(&router->outbound)) {
    size_t size_before = vec_size(&router->outbound);
    flowie_cluster_publish_router_outbound_t *entry =
        (flowie_cluster_publish_router_outbound_t *)vec_at(
            &router->outbound, index);
    rc = flowie_cluster_publish_egress_pump(entry->egress);
    if (rc != SALTS_OK) return rc;
    rc = flowie_cluster_publish_router_try_propose(router, entry, index);
    if (rc != SALTS_OK) return rc;
    if (vec_size(&router->outbound) == size_before) ++index;
  }
  return SALTS_OK;
}

size_t flowie_cluster_publish_router_outbound_count(
    const flowie_cluster_publish_router_t *router) {
  return router ? vec_size(&router->outbound) : 0u;
}
