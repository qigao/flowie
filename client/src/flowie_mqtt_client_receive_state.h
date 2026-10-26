/* Internal bounded QoS1 receive state, independent of network/ACK APIs.
 * All functions are called on Flowie's native MQTT Worker Owner. Foreign
 * threads enqueue settlement commands and never touch these slots.
 * Compile the SAME helpers into a pure C11 negative test to validate ABA,
 * duplicate PUBLISH, credit FULL, reconnect and packet ID reuse.
 */
#ifndef FLOWIE_MQTT_CLIENT_RECEIVE_STATE_H
#define FLOWIE_MQTT_CLIENT_RECEIVE_STATE_H

#include "flowie_mqtt_client.h"
#include <stdint.h>
#include <string.h>

#define FLOWIE_RECEIVE_DIGEST_BYTES 32u
typedef struct flowie_receive_slot_s {
  uint64_t delivery_sequence;
  uint16_t packet_id;
  uint8_t digest[FLOWIE_RECEIVE_DIGEST_BYTES];
  uint8_t occupied;
} flowie_receive_slot_t;

typedef struct flowie_receive_state_s {
  uint64_t session_generation;
  uint64_t next_sequence;
  size_t capacity;
  flowie_receive_slot_t slots[FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING];
} flowie_receive_state_t;

enum {
  FLOWIE_RECEIVE_REJECT = -1,
  FLOWIE_RECEIVE_FULL = 0,
  FLOWIE_RECEIVE_NEW = 1,
  FLOWIE_RECEIVE_DUPLICATE_PENDING = 2
};

static inline void flowie_receive_state_invalidate(flowie_receive_state_t *s) {
  if (!s) return;
  memset(s->slots, 0, sizeof(s->slots));
}

/* A new MQTT session fences all tokens of any prior CNet connection. */
static inline int flowie_receive_state_start(flowie_receive_state_t *s) {
  if (!s || !s->capacity ||
      s->capacity > FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING ||
      s->session_generation == UINT64_MAX)
    return FLOWIE_RECEIVE_REJECT;
  ++s->session_generation;
  s->next_sequence = 0u;
  flowie_receive_state_invalidate(s);
  return FLOWIE_RECEIVE_NEW;
}

static inline int flowie_receive_state_offer(
    flowie_receive_state_t *s, uint16_t packet_id, int duplicate,
    const uint8_t digest[FLOWIE_RECEIVE_DIGEST_BYTES],
    flowie_mqtt_client_inbound_token_t *out) {
  size_t i, empty = FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING;
  if (out) memset(out, 0, sizeof(*out));
  if (!s || !out || !digest || !s->session_generation || !s->capacity ||
      s->capacity > FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING ||
      !packet_id || (duplicate != 0 && duplicate != 1))
    return FLOWIE_RECEIVE_REJECT;
  for (i = 0u; i < s->capacity; ++i) {
    if (s->slots[i].occupied &&
        s->slots[i].packet_id == packet_id) {
      /* MQTT DUP does not authorize a DIFFERENT message to reuse the
       * pending packet ID. SHA256 covers the complete parsed PUBLISH body
       * including topic, packet ID, properties and payload, but excludes
       * the mutable DUP flag in the fixed header. */
      unsigned int difference = 0u;
      size_t j;
      for (j = 0u; j < FLOWIE_RECEIVE_DIGEST_BYTES; ++j)
        difference |= (unsigned int)(s->slots[i].digest[j] ^ digest[j]);
      return duplicate && difference == 0u
                 ? FLOWIE_RECEIVE_DUPLICATE_PENDING
                 : FLOWIE_RECEIVE_REJECT;
    }
    if (!s->slots[i].occupied && empty == FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING)
      empty = i;
  }
  if (empty == FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING)
    return FLOWIE_RECEIVE_FULL;
  if (s->next_sequence == UINT64_MAX)
    return FLOWIE_RECEIVE_REJECT;
  ++s->next_sequence;
  s->slots[empty].packet_id = packet_id;
  memcpy(s->slots[empty].digest, digest, FLOWIE_RECEIVE_DIGEST_BYTES);
  s->slots[empty].delivery_sequence = s->next_sequence;
  s->slots[empty].occupied = 1u;
  out->session_generation = s->session_generation;
  out->delivery_sequence = s->next_sequence;
  out->packet_id = packet_id;
  return FLOWIE_RECEIVE_NEW;
}

static inline int flowie_receive_state_contains(
    const flowie_receive_state_t *s,
    flowie_mqtt_client_inbound_token_t token) {
  size_t i;
  if (!s || token.session_generation != s->session_generation ||
      !token.session_generation || !token.delivery_sequence || !token.packet_id)
    return 0;
  for (i = 0u; i < s->capacity; ++i) {
    const flowie_receive_slot_t *slot = &s->slots[i];
    if (slot->occupied && slot->packet_id == token.packet_id &&
        slot->delivery_sequence == token.delivery_sequence)
      return 1;
  }
  return 0;
}

static inline int flowie_receive_state_retire(
    flowie_receive_state_t *s,
    flowie_mqtt_client_inbound_token_t token) {
  size_t i;
  if (!flowie_receive_state_contains(s, token)) return 0;
  for (i = 0u; i < s->capacity; ++i) {
    if (s->slots[i].occupied &&
        s->slots[i].packet_id == token.packet_id &&
        s->slots[i].delivery_sequence == token.delivery_sequence) {
      memset(&s->slots[i], 0, sizeof(s->slots[i]));
      return 1;
    }
  }
  return 0;
}

#endif
