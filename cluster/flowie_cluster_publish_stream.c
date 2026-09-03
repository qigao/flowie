#include "flowie_cluster_publish_stream_internal.h"

#include "salts_error.h"

#include <stdlib.h>
#include <string.h>

struct flowie_cluster_publish_stream_sender_s {
  tr_raft_data_stream_sender_t *stream;
  size_t max_event_bytes;
};

struct flowie_cluster_publish_stream_receiver_s {
  tr_raft_data_stream_receiver_t *stream;
  size_t max_event_bytes;
  flowie_cluster_publish_stream_commit_fn commit;
  void *commit_ctx;
  uint8_t *event;
  size_t event_size;
  size_t used;
};

struct flowie_cluster_publish_quorum_s {
  tr_raft_data_quorum_t *quorum;
};

static int flowie_cluster_publish_stream_sink_begin(
    void *ctx, tr_raft_node_id_t leader_id, tr_raft_term_t term,
    uint64_t stream_id, uint64_t stream_size,
    const uint8_t digest[TR_RAFT_WIRE_DATA_DIGEST_SIZE]) {
  flowie_cluster_publish_stream_receiver_t *receiver =
      (flowie_cluster_publish_stream_receiver_t *)ctx;
  (void)leader_id;
  (void)term;
  (void)stream_id;
  (void)digest;
  if (!receiver || stream_size == 0u || stream_size > receiver->max_event_bytes ||
      stream_size > SIZE_MAX)
    return SALTS_EMSGSIZE;
  free(receiver->event);
  receiver->event = (uint8_t *)malloc((size_t)stream_size);
  if (!receiver->event) return SALTS_ENOMEM;
  receiver->event_size = (size_t)stream_size;
  receiver->used = 0u;
  return SALTS_OK;
}

static int flowie_cluster_publish_stream_sink_write(void *ctx, uint64_t offset,
                                                     const uint8_t *data,
                                                     size_t size) {
  flowie_cluster_publish_stream_receiver_t *receiver =
      (flowie_cluster_publish_stream_receiver_t *)ctx;
  if (!receiver || !receiver->event || !data || offset != receiver->used ||
      size > receiver->event_size - receiver->used)
    return SALTS_EPROTO;
  memcpy(receiver->event + receiver->used, data, size);
  receiver->used += size;
  return SALTS_OK;
}

static int flowie_cluster_publish_stream_sink_commit(void *ctx) {
  flowie_cluster_publish_stream_receiver_t *receiver =
      (flowie_cluster_publish_stream_receiver_t *)ctx;
  flowie_cluster_publish_event_view_t event = FLOWIE_CLUSTER_PUBLISH_EVENT_VIEW_INIT;
  int rc;
  if (!receiver || !receiver->event || receiver->used != receiver->event_size)
    return SALTS_EPROTO;
  rc = flowie_cluster_publish_event_decode(receiver->event, receiver->event_size,
                                           receiver->max_event_bytes, &event);
  if (rc == SALTS_OK) rc = receiver->commit(receiver->commit_ctx, &event);
  return rc;
}

static void flowie_cluster_publish_stream_sink_abort(void *ctx) {
  flowie_cluster_publish_stream_receiver_t *receiver =
      (flowie_cluster_publish_stream_receiver_t *)ctx;
  if (!receiver) return;
  free(receiver->event);
  receiver->event = NULL;
  receiver->event_size = 0u;
  receiver->used = 0u;
}

int flowie_cluster_publish_stream_sender_create(
    const flowie_cluster_publish_stream_sender_config_t *config,
    flowie_cluster_publish_stream_sender_t **out) {
  flowie_cluster_publish_stream_sender_t *sender;
  tr_raft_data_stream_sender_config_t stream_config;
  int rc;
  if (out) *out = NULL;
  if (!config || !out || config->self_id == 0u || config->peer_id == 0u ||
      config->self_id == config->peer_id || config->max_event_bytes == 0u ||
      config->max_event_bytes > TR_RAFT_WIRE_MAX_DATA_STREAM_BYTES)
    return SALTS_EINVAL;
  sender = (flowie_cluster_publish_stream_sender_t *)calloc(1u, sizeof(*sender));
  if (!sender) return SALTS_ENOMEM;
  memset(&stream_config, 0, sizeof(stream_config));
  stream_config.self_id = config->self_id;
  stream_config.peer_id = config->peer_id;
  stream_config.max_stream_bytes = config->max_event_bytes;
  stream_config.chunk_size = config->chunk_size != 0u
                                 ? config->chunk_size
                                 : TR_RAFT_WIRE_MAX_DATA_CHUNK_BYTES;
  stream_config.max_inflight_chunks =
      config->max_inflight_chunks != 0u
          ? config->max_inflight_chunks
          : TR_RAFT_DATA_STREAM_RECOMMENDED_INFLIGHT_CHUNKS;
  rc = tr_raft_data_stream_sender_create(&stream_config, &sender->stream);
  if (rc != SALTS_OK) {
    free(sender);
    return rc;
  }
  sender->max_event_bytes = config->max_event_bytes;
  *out = sender;
  return SALTS_OK;
}

void flowie_cluster_publish_stream_sender_destroy(
    flowie_cluster_publish_stream_sender_t *sender) {
  if (!sender) return;
  tr_raft_data_stream_sender_destroy(sender->stream);
  free(sender);
}

int flowie_cluster_publish_stream_sender_begin(
    flowie_cluster_publish_stream_sender_t *sender, tr_raft_term_t term,
    uint64_t stream_id, const void *event, size_t event_size) {
  flowie_cluster_publish_event_view_t decoded = FLOWIE_CLUSTER_PUBLISH_EVENT_VIEW_INIT;
  int rc;
  if (!sender || term == 0u || stream_id == 0u || !event || event_size == 0u ||
      event_size > sender->max_event_bytes)
    return SALTS_EINVAL;
  rc = flowie_cluster_publish_event_decode(event, event_size,
                                           sender->max_event_bytes, &decoded);
  return rc == SALTS_OK
             ? tr_raft_data_stream_sender_begin(sender->stream, term, stream_id,
                                                (const uint8_t *)event, event_size)
             : rc;
}

int flowie_cluster_publish_stream_sender_next(
    flowie_cluster_publish_stream_sender_t *sender,
    tr_raft_data_chunk_t *out_chunk) {
  if (!sender || !out_chunk) return SALTS_EINVAL;
  return tr_raft_data_stream_sender_next(sender->stream, out_chunk);
}

int flowie_cluster_publish_stream_sender_acknowledge(
    flowie_cluster_publish_stream_sender_t *sender,
    const tr_raft_data_ack_t *ack) {
  if (!sender || !ack) return SALTS_EINVAL;
  return tr_raft_data_stream_sender_acknowledge(sender->stream, ack);
}

int flowie_cluster_publish_stream_sender_cancel(
    flowie_cluster_publish_stream_sender_t *sender, uint64_t stream_offset) {
  if (!sender) return SALTS_EINVAL;
  return tr_raft_data_stream_sender_cancel(sender->stream, stream_offset);
}

int flowie_cluster_publish_stream_sender_status(
    const flowie_cluster_publish_stream_sender_t *sender,
    tr_raft_data_stream_sender_status_t *out_status) {
  if (!sender || !out_status) return SALTS_EINVAL;
  return tr_raft_data_stream_sender_get_status(sender->stream, out_status);
}

int flowie_cluster_publish_stream_receiver_create(
    const flowie_cluster_publish_stream_receiver_config_t *config,
    flowie_cluster_publish_stream_receiver_t **out) {
  flowie_cluster_publish_stream_receiver_t *receiver;
  tr_raft_data_stream_receiver_config_t stream_config;
  int rc;
  if (out) *out = NULL;
  if (!config || !out || config->self_id == 0u || config->max_event_bytes == 0u ||
      config->max_event_bytes > TR_RAFT_WIRE_MAX_DATA_STREAM_BYTES || !config->commit)
    return SALTS_EINVAL;
  receiver = (flowie_cluster_publish_stream_receiver_t *)calloc(1u, sizeof(*receiver));
  if (!receiver) return SALTS_ENOMEM;
  receiver->max_event_bytes = config->max_event_bytes;
  receiver->commit = config->commit;
  receiver->commit_ctx = config->commit_ctx;
  memset(&stream_config, 0, sizeof(stream_config));
  stream_config.self_id = config->self_id;
  stream_config.max_stream_bytes = config->max_event_bytes;
  stream_config.sink.begin = flowie_cluster_publish_stream_sink_begin;
  stream_config.sink.write = flowie_cluster_publish_stream_sink_write;
  stream_config.sink.commit = flowie_cluster_publish_stream_sink_commit;
  stream_config.sink.abort = flowie_cluster_publish_stream_sink_abort;
  stream_config.sink.context = receiver;
  rc = tr_raft_data_stream_receiver_create(&stream_config, &receiver->stream);
  if (rc != SALTS_OK) {
    free(receiver);
    return rc;
  }
  *out = receiver;
  return SALTS_OK;
}

void flowie_cluster_publish_stream_receiver_destroy(
    flowie_cluster_publish_stream_receiver_t *receiver) {
  if (!receiver) return;
  tr_raft_data_stream_receiver_destroy(receiver->stream);
  free(receiver->event);
  free(receiver);
}

int flowie_cluster_publish_stream_receiver_handle(
    flowie_cluster_publish_stream_receiver_t *receiver,
    const tr_raft_data_chunk_t *chunk, tr_raft_data_ack_t *out_ack,
    int *out_committed) {
  tr_raft_data_stream_receive_result_t result;
  int rc;
  if (out_committed) *out_committed = 0;
  if (!receiver || !chunk || !out_ack || !out_committed) return SALTS_EINVAL;
  memset(&result, 0, sizeof(result));
  rc = tr_raft_data_stream_receiver_handle(receiver->stream, chunk, &result);
  if (rc != SALTS_OK) return rc;
  *out_ack = result.ack;
  *out_committed = result.committed ? 1 : 0;
  return SALTS_OK;
}

int flowie_cluster_publish_quorum_create(
    tr_raft_node_id_t self_id, const tr_raft_conf_t *configuration,
    const tr_raft_data_chunk_t *first_chunk,
    flowie_cluster_publish_quorum_t **out) {
  flowie_cluster_publish_quorum_t *owner;
  tr_raft_data_quorum_config_t config;
  int rc;
  if (out) *out = NULL;
  if (!out || self_id == 0u || !configuration || !first_chunk ||
      first_chunk->from != self_id || first_chunk->term == 0u ||
      first_chunk->stream_id == 0u || first_chunk->stream_offset != 0u ||
      first_chunk->stream_size == 0u)
    return SALTS_EINVAL;
  owner = (flowie_cluster_publish_quorum_t *)calloc(1u, sizeof(*owner));
  if (!owner) return SALTS_ENOMEM;
  memset(&config, 0, sizeof(config));
  config.self_id = self_id;
  config.term = first_chunk->term;
  config.configuration = *configuration;
  config.descriptor.stream_id = first_chunk->stream_id;
  config.descriptor.stream_size = first_chunk->stream_size;
  memcpy(config.descriptor.stream_digest, first_chunk->stream_digest,
         sizeof(config.descriptor.stream_digest));
  rc = tr_raft_data_quorum_create(&config, &owner->quorum);
  if (rc != SALTS_OK) {
    free(owner);
    return rc;
  }
  *out = owner;
  return SALTS_OK;
}

void flowie_cluster_publish_quorum_destroy(
    flowie_cluster_publish_quorum_t *quorum) {
  if (!quorum) return;
  tr_raft_data_quorum_destroy(quorum->quorum);
  free(quorum);
}

int flowie_cluster_publish_quorum_mark_local_durable(
    flowie_cluster_publish_quorum_t *quorum) {
  return quorum ? tr_raft_data_quorum_mark_local_durable(quorum->quorum)
                : SALTS_EINVAL;
}

int flowie_cluster_publish_quorum_acknowledge(
    flowie_cluster_publish_quorum_t *quorum, const tr_raft_data_ack_t *ack) {
  if (!quorum || !ack) return SALTS_EINVAL;
  return tr_raft_data_quorum_acknowledge(quorum->quorum, ack);
}

int flowie_cluster_publish_quorum_make_proposal(
    const flowie_cluster_publish_quorum_t *quorum, uint64_t command_id,
    uint8_t descriptor[TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE],
    tr_raft_proposal_t *out_proposal) {
  if (!quorum || command_id == 0u || !descriptor || !out_proposal)
    return SALTS_EINVAL;
  return tr_raft_data_quorum_make_proposal(quorum->quorum, command_id,
                                           descriptor, out_proposal);
}
