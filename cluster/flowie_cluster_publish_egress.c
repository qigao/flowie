#include "flowie_stl_error_internal.h"

#include <turbostl/deque.h>
#include <turbostl/hash_map.h>
#include <turbostl/hash_set.h>
#include <turbostl/vec.h>

#include "flowie_cluster_publish_egress_internal.h"

#include "turbo_error.h"
#include <turbostl/vec.h>

#include <stdlib.h>
#include <string.h>

typedef struct flowie_cluster_publish_egress_leg_s {
  tr_raft_node_id_t peer_id;
  flowie_cluster_publish_stream_sender_t *sender;
} flowie_cluster_publish_egress_leg_t;

struct flowie_cluster_publish_egress_s {
  flowie_cluster_publish_egress_config_t config;
  tstr event;
  vec_t legs;
  flowie_cluster_publish_quorum_t *quorum;
  int local_durable;
};

static void flowie_cluster_publish_egress_free(
    flowie_cluster_publish_egress_t *egress) {
  size_t index;
  if (!egress) return;
  for (index = 0u; index < vec_size(&egress->legs); ++index) {
    flowie_cluster_publish_egress_leg_t *leg =
        (flowie_cluster_publish_egress_leg_t *)vec_at(&egress->legs,
                                                            index);
    flowie_cluster_publish_stream_sender_destroy(leg->sender);
  }
  vec_destroy(&egress->legs);
  flowie_cluster_publish_quorum_destroy(egress->quorum);
  tstr_free(egress->event);
  free(egress);
}

static flowie_cluster_publish_egress_leg_t *
flowie_cluster_publish_egress_find(flowie_cluster_publish_egress_t *egress,
                                   tr_raft_node_id_t peer_id) {
  size_t index;
  for (index = 0u; index < vec_size(&egress->legs); ++index) {
    flowie_cluster_publish_egress_leg_t *leg =
        (flowie_cluster_publish_egress_leg_t *)vec_at(&egress->legs,
                                                            index);
    if (leg->peer_id == peer_id) return leg;
  }
  return NULL;
}

int flowie_cluster_publish_egress_create(
    const flowie_cluster_publish_egress_config_t *config, tstr *event,
    flowie_cluster_publish_egress_t **out) {
  flowie_cluster_publish_egress_t *egress;
  size_t index;
  size_t self_count = 0u;
  int rc;
  if (out) *out = NULL;
  if (!config || !event || !*event || !out || config->self_id == 0u ||
      config->term == 0u || config->stream_id == 0u ||
      config->configuration.member_count == 0u ||
      config->max_event_bytes == 0u ||
      tstr_len(*event) > config->max_event_bytes || !config->enqueue)
    return TURBO_EINVAL;
  egress = (flowie_cluster_publish_egress_t *)calloc(1u, sizeof(*egress));
  if (!egress) return TURBO_ENOMEM;
  egress->config = *config;
  rc = flowie_stl_error(vec_init_bytes(&egress->legs, sizeof(flowie_cluster_publish_egress_leg_t), _Alignof(flowie_cluster_publish_egress_leg_t), SIZE_MAX));
  if (rc != TURBO_OK) goto fail;
  rc = flowie_stl_error(vec_reserve(&egress->legs,
                         config->configuration.member_count - 1u));
  if (rc != TURBO_OK) goto fail;
  for (index = 0u; index < config->configuration.member_count; ++index) {
    const tr_raft_conf_member_t *member =
        &config->configuration.members[index];
    flowie_cluster_publish_stream_sender_config_t sender_config;
    flowie_cluster_publish_egress_leg_t leg;
    if (member->node_id == config->self_id) {
      ++self_count;
      continue;
    }
    memset(&sender_config, 0, sizeof(sender_config));
    memset(&leg, 0, sizeof(leg));
    sender_config.self_id = config->self_id;
    sender_config.peer_id = member->node_id;
    sender_config.max_event_bytes = config->max_event_bytes;
    sender_config.chunk_size = config->chunk_size;
    sender_config.max_inflight_chunks = config->max_inflight_chunks;
    rc = flowie_cluster_publish_stream_sender_create(&sender_config,
                                                       &leg.sender);
    if (rc != TURBO_OK) goto fail;
    rc = flowie_cluster_publish_stream_sender_begin(
        leg.sender, config->term, config->stream_id, *event,
        tstr_len(*event));
    if (rc != TURBO_OK) {
      flowie_cluster_publish_stream_sender_destroy(leg.sender);
      goto fail;
    }
    leg.peer_id = member->node_id;
    rc = flowie_stl_error(vec_push(&egress->legs, &leg));
    if (rc != TURBO_OK) {
      flowie_cluster_publish_stream_sender_destroy(leg.sender);
      goto fail;
    }
  }
  if (self_count != 1u || vec_empty(&egress->legs)) {
    rc = TURBO_EINVAL;
    goto fail;
  }
  egress->event = *event;
  *event = NULL;
  *out = egress;
  return TURBO_OK;
fail:
  flowie_cluster_publish_egress_free(egress);
  return rc;
}

void flowie_cluster_publish_egress_destroy(
    flowie_cluster_publish_egress_t *egress) {
  flowie_cluster_publish_egress_free(egress);
}

int flowie_cluster_publish_egress_mark_local_durable(
    flowie_cluster_publish_egress_t *egress) {
  int rc;
  if (!egress) return TURBO_EINVAL;
  egress->local_durable = 1;
  if (!egress->quorum) return TURBO_OK;
  rc = flowie_cluster_publish_quorum_mark_local_durable(egress->quorum);
  if (rc != TURBO_OK) egress->local_durable = 0;
  return rc;
}

static int flowie_cluster_publish_egress_pump_leg(
    flowie_cluster_publish_egress_t *egress,
    flowie_cluster_publish_egress_leg_t *leg) {
  for (;;) {
    tr_raft_coronet_payload_t payload;
    int rc;
    memset(&payload, 0, sizeof(payload));
    payload.kind = TR_RAFT_WIRE_PAYLOAD_DATA_CHUNK;
    rc = flowie_cluster_publish_stream_sender_next(
        leg->sender, &payload.data.data_chunk);
    if (rc == TURBO_EBUSY || rc == TURBO_ENOENT) return TURBO_OK;
    if (rc != TURBO_OK) return rc;
    if (!egress->quorum) {
      rc = flowie_cluster_publish_quorum_create(
          egress->config.self_id, &egress->config.configuration,
          &payload.data.data_chunk, &egress->quorum);
      if (rc == TURBO_OK && egress->local_durable)
        rc = flowie_cluster_publish_quorum_mark_local_durable(egress->quorum);
      if (rc != TURBO_OK) {
        (void)flowie_cluster_publish_stream_sender_cancel(
            leg->sender, payload.data.data_chunk.stream_offset);
        return rc;
      }
    }
    rc = egress->config.enqueue(egress->config.enqueue_ctx, &payload);
    if (rc != TURBO_OK) {
      int cancel_rc = flowie_cluster_publish_stream_sender_cancel(
          leg->sender, payload.data.data_chunk.stream_offset);
      return cancel_rc == TURBO_OK ? rc : cancel_rc;
    }
  }
}

int flowie_cluster_publish_egress_pump(
    flowie_cluster_publish_egress_t *egress) {
  size_t index;
  int rc;
  if (!egress) return TURBO_EINVAL;
  for (index = 0u; index < vec_size(&egress->legs); ++index) {
    flowie_cluster_publish_egress_leg_t *leg =
        (flowie_cluster_publish_egress_leg_t *)vec_at(&egress->legs,
                                                            index);
    rc = flowie_cluster_publish_egress_pump_leg(egress, leg);
    if (rc != TURBO_OK) return rc;
  }
  return TURBO_OK;
}

int flowie_cluster_publish_egress_acknowledge(
    flowie_cluster_publish_egress_t *egress,
    const tr_raft_data_ack_t *ack) {
  flowie_cluster_publish_egress_leg_t *leg;
  int rc;
  if (!egress || !ack || ack->to != egress->config.self_id ||
      ack->term != egress->config.term ||
      ack->stream_id != egress->config.stream_id)
    return TURBO_EINVAL;
  leg = flowie_cluster_publish_egress_find(egress, ack->from);
  if (!leg) return TURBO_ENOENT;
  rc = flowie_cluster_publish_stream_sender_acknowledge(leg->sender, ack);
  if (rc == TURBO_OK && ack->durable) {
    if (!egress->quorum) return TURBO_EPROTO;
    rc = flowie_cluster_publish_quorum_acknowledge(egress->quorum, ack);
  }
  if (rc == TURBO_OK) rc = flowie_cluster_publish_egress_pump_leg(egress, leg);
  return rc;
}

int flowie_cluster_publish_egress_make_proposal(
    const flowie_cluster_publish_egress_t *egress, uint64_t command_id,
    uint8_t descriptor[TR_RAFT_DATA_DESCRIPTOR_ENCODED_SIZE],
    tr_raft_proposal_t *out_proposal) {
  if (!egress || !egress->quorum) return TURBO_EBUSY;
  return flowie_cluster_publish_quorum_make_proposal(
      egress->quorum, command_id, descriptor, out_proposal);
}
