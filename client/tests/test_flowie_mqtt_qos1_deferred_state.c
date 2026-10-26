#include "../src/flowie_mqtt_client_receive_state.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void) {
  flowie_receive_state_t state = {0};
  flowie_mqtt_client_inbound_token_t accepted[FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING] = {{0}};
  flowie_mqtt_client_inbound_token_t candidate = {0};
  flowie_mqtt_client_inbound_token_t old;
  size_t i;
  state.capacity = 2u;

  /* Nothing is admissible before a real native MQTT CONNECT succeeds. */
  assert(flowie_receive_state_offer(&state, 1u, 0, &candidate) ==
         FLOWIE_RECEIVE_REJECT);
  assert(!candidate.delivery_sequence);
  assert(flowie_receive_state_start(&state) == FLOWIE_RECEIVE_NEW);

  assert(flowie_receive_state_offer(&state, 11u, 0, &accepted[0]) ==
         FLOWIE_RECEIVE_NEW);
  assert(flowie_receive_state_offer(&state, 12u, 0, &accepted[1]) ==
         FLOWIE_RECEIVE_NEW);
  assert(accepted[0].session_generation == state.session_generation &&
         accepted[0].delivery_sequence == 1u && accepted[0].packet_id == 11u);
  assert(accepted[1].delivery_sequence == 2u);
  /* Duplicate in-flight QoS1 MUST NOT be ACKed early nor callback-delivered
   * again. A second PUBLISH with same packet ID but no DUP is a violation. */
  assert(flowie_receive_state_offer(&state, 11u, 1, &candidate) ==
         FLOWIE_RECEIVE_DUPLICATE_PENDING);
  assert(!candidate.session_generation);
  assert(flowie_receive_state_offer(&state, 11u, 0, &candidate) ==
         FLOWIE_RECEIVE_REJECT);
  assert(flowie_receive_state_offer(&state, 13u, 0, &candidate) ==
         FLOWIE_RECEIVE_FULL);
  assert(!candidate.session_generation);
  assert(flowie_receive_state_offer(&state, 0u, 0, &candidate) ==
         FLOWIE_RECEIVE_REJECT);

  assert(!flowie_receive_state_retire(
      &state, (flowie_mqtt_client_inbound_token_t){
        accepted[0].session_generation, accepted[0].delivery_sequence + 1u,
        accepted[0].packet_id}));
  assert(flowie_receive_state_contains(&state, accepted[0]));
  assert(flowie_receive_state_retire(&state, accepted[0]));
  assert(!flowie_receive_state_retire(&state, accepted[0]));
  assert(flowie_receive_state_offer(&state, 11u, 0, &candidate) ==
         FLOWIE_RECEIVE_NEW);
  assert(candidate.delivery_sequence == 3u);
  assert(!flowie_receive_state_retire(&state, accepted[0]));
  assert(flowie_receive_state_contains(&state, candidate));
  old = candidate;

  /* Reconnect closes the old delivery owner. Packet ID reuse never
   * authorizes a previous token to settle the NEW connection. */
  assert(flowie_receive_state_start(&state) == FLOWIE_RECEIVE_NEW);
  assert(!flowie_receive_state_contains(&state, old));
  assert(!flowie_receive_state_retire(&state, old));
  assert(!flowie_receive_state_contains(&state, accepted[1]));
  assert(flowie_receive_state_offer(&state, 11u, 0, &candidate) ==
         FLOWIE_RECEIVE_NEW);
  assert(candidate.session_generation == old.session_generation + 1u);
  assert(candidate.delivery_sequence == 1u &&
         candidate.packet_id == old.packet_id);
  assert(!flowie_receive_state_retire(&state, old));

  /* FULL for a larger configured window is bounded to exactly 16, and all
   * terminal releases restore capacity once (never twice). */
  state.capacity = FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING;
  assert(flowie_receive_state_start(&state) == FLOWIE_RECEIVE_NEW);
  for (i = 0u; i < state.capacity; ++i) {
    assert(flowie_receive_state_offer(&state, (uint16_t)(100u+i), 0,
                                      &accepted[i]) == FLOWIE_RECEIVE_NEW);
  }
  assert(flowie_receive_state_offer(&state, 999u, 0, &candidate) ==
         FLOWIE_RECEIVE_FULL);
  for (i = 0u; i < state.capacity; ++i) {
    assert(flowie_receive_state_retire(&state, accepted[i]));
    assert(!flowie_receive_state_retire(&state, accepted[i]));
  }
  assert(flowie_receive_state_offer(&state, 999u, 0, &candidate) ==
         FLOWIE_RECEIVE_NEW);

  state.next_sequence = UINT64_MAX;
  assert(flowie_receive_state_offer(&state, 998u, 0, &old) ==
         FLOWIE_RECEIVE_REJECT);
  state.session_generation = UINT64_MAX;
  assert(flowie_receive_state_start(&state) == FLOWIE_RECEIVE_REJECT);

  /* Invalid max_pending may not silently become an unbounded mode. */
  memset(&state, 0, sizeof(state));
  state.capacity = FLOWIE_MQTT_CLIENT_RECEIVE_QOS1_MAX_PENDING + 1u;
  assert(flowie_receive_state_start(&state) == FLOWIE_RECEIVE_REJECT);
  state.capacity = 0u;
  assert(flowie_receive_state_start(&state) == FLOWIE_RECEIVE_REJECT);

  puts("flowie.mqtt.qos1_deferred_state: PASS bounded credit, duplicate, "
       "session generation, stale ACK and packet ID reuse");
  return 0;
}
