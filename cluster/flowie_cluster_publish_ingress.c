#include "flowie_stl_error_internal.h"

#include <cstl.h>
#include <cstl.h>
#include <cstl.h>
#include <cstl.h>

#include "flowie_cluster_publish_ingress_internal.h"

#include "salts_error.h"
#include <cstl.h>

#include <stdlib.h>
#include <string.h>

typedef struct flowie_cluster_publish_ingress_entry_s {
  tr_raft_node_id_t from;
  uint64_t stream_id;
  flowie_cluster_publish_stream_receiver_t *receiver;
  tr_raft_data_ack_t pending_ack;
  int ack_pending;
  int committed;
} flowie_cluster_publish_ingress_entry_t;

struct flowie_cluster_publish_ingress_s {
  tr_raft_node_id_t self_id;
  size_t max_event_bytes;
  size_t max_active_streams;
  flowie_cluster_publish_stream_commit_fn commit;
  void *commit_ctx;
  flowie_cluster_publish_ingress_enqueue_fn enqueue;
  void *enqueue_ctx;
  vec_t entries;
};

static flowie_cluster_publish_ingress_entry_t *
flowie_cluster_publish_ingress_find(flowie_cluster_publish_ingress_t *ingress,
                                    tr_raft_node_id_t from,
                                    uint64_t stream_id, size_t *out_index) {
  size_t index;
  for (index = 0u; index < vec_size(&ingress->entries); ++index) {
    flowie_cluster_publish_ingress_entry_t *entry =
        (flowie_cluster_publish_ingress_entry_t *)vec_at(
            &ingress->entries, index);
    if (entry->from == from && entry->stream_id == stream_id) {
      if (out_index) *out_index = index;
      return entry;
    }
  }
  return NULL;
}

static void flowie_cluster_publish_ingress_remove(
    flowie_cluster_publish_ingress_t *ingress, size_t index) {
  flowie_cluster_publish_ingress_entry_t removed;
  if (flowie_stl_error(vec_swap_remove(&ingress->entries, index, &removed)) == SALTS_OK)
    flowie_cluster_publish_stream_receiver_destroy(removed.receiver);
}

static int flowie_cluster_publish_ingress_send_ack(
    flowie_cluster_publish_ingress_t *ingress,
    flowie_cluster_publish_ingress_entry_t *entry) {
  tr_raft_transport_payload_t payload;
  int rc;
  memset(&payload, 0, sizeof(payload));
  payload.kind = TR_RAFT_WIRE_PAYLOAD_DATA_ACK;
  payload.data.data_ack = entry->pending_ack;
  rc = ingress->enqueue(ingress->enqueue_ctx, &payload);
  if (rc == SALTS_OK) entry->ack_pending = 0;
  return rc;
}

int flowie_cluster_publish_ingress_create(
    const flowie_cluster_publish_ingress_config_t *config,
    flowie_cluster_publish_ingress_t **out) {
  flowie_cluster_publish_ingress_t *ingress;
  int rc;
  if (out) *out = NULL;
  if (!config || !out || config->self_id == 0u ||
      config->max_event_bytes == 0u ||
      config->max_event_bytes > TR_RAFT_WIRE_MAX_DATA_STREAM_BYTES ||
      config->max_active_streams == 0u || !config->commit || !config->enqueue)
    return SALTS_EINVAL;
  ingress = (flowie_cluster_publish_ingress_t *)calloc(1u, sizeof(*ingress));
  if (!ingress) return SALTS_ENOMEM;
  ingress->self_id = config->self_id;
  ingress->max_event_bytes = config->max_event_bytes;
  ingress->max_active_streams = config->max_active_streams;
  ingress->commit = config->commit;
  ingress->commit_ctx = config->commit_ctx;
  ingress->enqueue = config->enqueue;
  ingress->enqueue_ctx = config->enqueue_ctx;
  rc = flowie_stl_error(vec_init_bytes(&ingress->entries, sizeof(flowie_cluster_publish_ingress_entry_t), _Alignof(flowie_cluster_publish_ingress_entry_t), SIZE_MAX));
  if (rc == SALTS_OK)
    rc = flowie_stl_error(vec_reserve(&ingress->entries, config->max_active_streams));
  if (rc != SALTS_OK) {
    vec_destroy(&ingress->entries);
    free(ingress);
    return rc;
  }
  *out = ingress;
  return SALTS_OK;
}

void flowie_cluster_publish_ingress_destroy(
    flowie_cluster_publish_ingress_t *ingress) {
  if (!ingress) return;
  while (!vec_empty(&ingress->entries))
    flowie_cluster_publish_ingress_remove(
        ingress, vec_size(&ingress->entries) - 1u);
  vec_destroy(&ingress->entries);
  free(ingress);
}

int flowie_cluster_publish_ingress_handle(
    flowie_cluster_publish_ingress_t *ingress,
    const tr_raft_transport_payload_t *payload) {
  const tr_raft_data_chunk_t *chunk;
  flowie_cluster_publish_ingress_entry_t *entry;
  size_t entry_index = 0u;
  int rc;
  int committed = 0;
  if (!ingress || !payload ||
      payload->kind != TR_RAFT_WIRE_PAYLOAD_DATA_CHUNK)
    return SALTS_EINVAL;
  chunk = &payload->data.data_chunk;
  if (chunk->to != ingress->self_id || chunk->from == 0u ||
      chunk->stream_id == 0u)
    return SALTS_EPROTO;
  entry = flowie_cluster_publish_ingress_find(
      ingress, chunk->from, chunk->stream_id, &entry_index);
  if (entry && entry->ack_pending) {
    uint64_t acknowledged_offset = entry->pending_ack.next_offset;
    rc = flowie_cluster_publish_ingress_send_ack(ingress, entry);
    if (rc != SALTS_OK) return rc;
    if (entry->committed) {
      flowie_cluster_publish_ingress_remove(ingress, entry_index);
      return SALTS_OK;
    }
    if (chunk->stream_offset < acknowledged_offset) return SALTS_OK;
  }
  if (!entry) {
    flowie_cluster_publish_stream_receiver_config_t receiver_config;
    flowie_cluster_publish_ingress_entry_t created;
    if (chunk->stream_offset != 0u) return SALTS_EPROTO;
    if (vec_size(&ingress->entries) >= ingress->max_active_streams)
      return SALTS_ENOSPC;
    memset(&created, 0, sizeof(created));
    memset(&receiver_config, 0, sizeof(receiver_config));
    receiver_config.self_id = ingress->self_id;
    receiver_config.max_event_bytes = ingress->max_event_bytes;
    receiver_config.commit = ingress->commit;
    receiver_config.commit_ctx = ingress->commit_ctx;
    rc = flowie_cluster_publish_stream_receiver_create(
        &receiver_config, &created.receiver);
    if (rc != SALTS_OK) return rc;
    created.from = chunk->from;
    created.stream_id = chunk->stream_id;
    rc = flowie_stl_error(vec_push(&ingress->entries, &created));
    if (rc != SALTS_OK) {
      flowie_cluster_publish_stream_receiver_destroy(created.receiver);
      return rc;
    }
    entry_index = vec_size(&ingress->entries) - 1u;
    entry = (flowie_cluster_publish_ingress_entry_t *)vec_at(
        &ingress->entries, entry_index);
  }
  rc = flowie_cluster_publish_stream_receiver_handle(
      entry->receiver, chunk, &entry->pending_ack, &committed);
  if (rc != SALTS_OK) return rc;
  entry->ack_pending = 1;
  entry->committed = committed;
  rc = flowie_cluster_publish_ingress_send_ack(ingress, entry);
  if (rc != SALTS_OK) return rc;
  if (entry->committed)
    flowie_cluster_publish_ingress_remove(ingress, entry_index);
  return SALTS_OK;
}

size_t flowie_cluster_publish_ingress_active_count(
    const flowie_cluster_publish_ingress_t *ingress) {
  return ingress ? vec_size(&ingress->entries) : 0u;
}
