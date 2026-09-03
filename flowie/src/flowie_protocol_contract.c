#include "flowie_protocol_contract.h"

#include <string.h>

static int flow_protocol_settlement_point_valid(flowie_protocol_settlement_point_t point) {
  return point >= FLOWIE_PROTOCOL_SETTLE_RECEIVED &&
         point <= FLOWIE_PROTOCOL_SETTLE_DURABLE;
}

static int flow_pattern_selector_valid(const flowie_pattern_selector_t *selector) {
  return selector && selector->size >= sizeof(*selector) &&
         selector->contract_version == FLOWIE_PROTOCOL_CONTRACT_VERSION;
}

static int flow_pattern_iterator_valid(
    const flowie_pattern_selection_iterator_t *iterator) {
  return iterator && iterator->size >= sizeof(*iterator) &&
         iterator->contract_version == FLOWIE_PROTOCOL_CONTRACT_VERSION &&
         (iterator->selection == FLOWIE_PATTERN_SELECT_FAN_OUT ||
          iterator->selection == FLOWIE_PATTERN_SELECT_ROUND_ROBIN) &&
         iterator->candidate_count != 0u && iterator->remaining <= iterator->candidate_count &&
         iterator->next_offset < iterator->candidate_count;
}

static int flow_pattern_exchange_valid(const flowie_pattern_exchange_t *exchange) {
  return exchange && exchange->size >= sizeof(*exchange) &&
         exchange->contract_version == FLOWIE_PROTOCOL_CONTRACT_VERSION;
}

static int flow_pattern_exchange_active_state(flowie_pattern_exchange_state_t state) {
  return state == FLOWIE_PATTERN_EXCHANGE_WAIT_REPLY ||
         state == FLOWIE_PATTERN_EXCHANGE_PROCESSING_REQUEST;
}

int flowie_pattern_role_validate(flowie_pattern_role_t role) {
  return role >= FLOWIE_PATTERN_PUBLISH && role <= FLOWIE_PATTERN_EXTENDED_SUBSCRIBE
             ? SALTS_OK
             : SALTS_EINVAL;
}

int flowie_pattern_roles_compatible(flowie_pattern_role_t local,
                                        flowie_pattern_role_t remote) {
  if (flowie_pattern_role_validate(local) != SALTS_OK ||
      flowie_pattern_role_validate(remote) != SALTS_OK)
    return 0;
  switch (local) {
  case FLOWIE_PATTERN_PUBLISH:
    return remote == FLOWIE_PATTERN_SUBSCRIBE ||
           remote == FLOWIE_PATTERN_EXTENDED_SUBSCRIBE;
  case FLOWIE_PATTERN_SUBSCRIBE:
    return remote == FLOWIE_PATTERN_PUBLISH ||
           remote == FLOWIE_PATTERN_EXTENDED_PUBLISH;
  case FLOWIE_PATTERN_PUSH:
    return remote == FLOWIE_PATTERN_PULL;
  case FLOWIE_PATTERN_PULL:
    return remote == FLOWIE_PATTERN_PUSH;
  case FLOWIE_PATTERN_ROUTE:
    return remote == FLOWIE_PATTERN_DEAL;
  case FLOWIE_PATTERN_DEAL:
    return remote == FLOWIE_PATTERN_ROUTE;
  case FLOWIE_PATTERN_PAIR:
    return remote == FLOWIE_PATTERN_PAIR;
  case FLOWIE_PATTERN_REQUEST:
    return remote == FLOWIE_PATTERN_REPLY;
  case FLOWIE_PATTERN_REPLY:
    return remote == FLOWIE_PATTERN_REQUEST;
  case FLOWIE_PATTERN_EXTENDED_PUBLISH:
    return remote == FLOWIE_PATTERN_SUBSCRIBE ||
           remote == FLOWIE_PATTERN_EXTENDED_SUBSCRIBE;
  case FLOWIE_PATTERN_EXTENDED_SUBSCRIBE:
    return remote == FLOWIE_PATTERN_PUBLISH ||
           remote == FLOWIE_PATTERN_EXTENDED_PUBLISH;
  default:
    return 0;
  }
}

int flowie_pattern_selector_init(flowie_pattern_selector_t *selector) {
  if (!selector) return SALTS_EINVAL;
  selector->size = sizeof(*selector);
  selector->contract_version = FLOWIE_PROTOCOL_CONTRACT_VERSION;
  atomic_init(&selector->cursor, 0u);
  return SALTS_OK;
}

int flowie_pattern_selection_begin(
    flowie_pattern_selector_t *selector, flowie_pattern_selection_t selection,
    size_t candidate_count, flowie_pattern_selection_iterator_t *iterator) {
  flowie_pattern_selection_iterator_t value = FLOWIE_PATTERN_SELECTION_ITERATOR_INIT;
  uint_fast64_t ticket = 0u;
  if (!flow_pattern_selector_valid(selector) || !iterator ||
      iterator->size < sizeof(*iterator) ||
      iterator->contract_version != FLOWIE_PROTOCOL_CONTRACT_VERSION ||
      (selection != FLOWIE_PATTERN_SELECT_FAN_OUT &&
       selection != FLOWIE_PATTERN_SELECT_ROUND_ROBIN))
    return SALTS_EINVAL;
  if (candidate_count == 0u) return SALTS_ENOENT;
  if (selection == FLOWIE_PATTERN_SELECT_ROUND_ROBIN)
    ticket = atomic_fetch_add_explicit(&selector->cursor, 1u, memory_order_relaxed);
  value.selection = selection;
  value.candidate_count = candidate_count;
  value.next_offset = selection == FLOWIE_PATTERN_SELECT_ROUND_ROBIN
                          ? (size_t)(ticket % candidate_count)
                          : 0u;
  value.remaining = candidate_count;
  *iterator = value;
  return SALTS_OK;
}

int flowie_pattern_selection_next(flowie_pattern_selection_iterator_t *iterator,
                                      size_t *candidate_index) {
  if (!flow_pattern_iterator_valid(iterator) || !candidate_index) return SALTS_EINVAL;
  if (iterator->remaining == 0u) return SALTS_ENOENT;
  *candidate_index = iterator->next_offset;
  iterator->next_offset += 1u;
  if (iterator->next_offset == iterator->candidate_count) iterator->next_offset = 0u;
  iterator->remaining -= 1u;
  return SALTS_OK;
}

int flowie_pattern_route_validate(const flowie_pattern_route_t *route) {
  return route && route->size >= sizeof(*route) &&
                 route->contract_version == FLOWIE_PROTOCOL_CONTRACT_VERSION &&
                 route->route_id != 0u && route->generation != 0u
             ? SALTS_OK
             : SALTS_EINVAL;
}

int flowie_pattern_routes_match(const flowie_pattern_route_t *requested,
                                    const flowie_pattern_route_t *candidate) {
  return flowie_pattern_route_validate(requested) == SALTS_OK &&
         flowie_pattern_route_validate(candidate) == SALTS_OK &&
         requested->route_id == candidate->route_id &&
         requested->generation == candidate->generation;
}

int flowie_pattern_exchange_init(flowie_pattern_exchange_t *exchange) {
  if (!exchange) return SALTS_EINVAL;
  exchange->size = sizeof(*exchange);
  exchange->contract_version = FLOWIE_PROTOCOL_CONTRACT_VERSION;
  atomic_init(&exchange->state, FLOWIE_PATTERN_EXCHANGE_READY);
  atomic_init(&exchange->correlation_id, 0u);
  return SALTS_OK;
}

int flowie_pattern_exchange_snapshot(const flowie_pattern_exchange_t *exchange,
                                         flowie_pattern_exchange_state_t *state,
                                         uint64_t *correlation_id) {
  int value;
  if (!flow_pattern_exchange_valid(exchange) || !state || !correlation_id) return SALTS_EINVAL;
  value = atomic_load_explicit(&exchange->state, memory_order_acquire);
  if (value < FLOWIE_PATTERN_EXCHANGE_READY ||
      value > FLOWIE_PATTERN_EXCHANGE_RESETTING)
    return SALTS_EPROTO;
  *state = (flowie_pattern_exchange_state_t)value;
  *correlation_id = atomic_load_explicit(&exchange->correlation_id, memory_order_acquire);
  return SALTS_OK;
}

int flowie_pattern_exchange_begin(flowie_pattern_exchange_t *exchange,
                                      flowie_pattern_exchange_state_t active_state,
                                      uint64_t correlation_id) {
  uint_fast64_t empty = 0u;
  int expected = FLOWIE_PATTERN_EXCHANGE_READY;
  if (!flow_pattern_exchange_valid(exchange) || !flow_pattern_exchange_active_state(active_state) ||
      correlation_id == 0u)
    return SALTS_EINVAL;
  if (!atomic_compare_exchange_strong_explicit(&exchange->correlation_id, &empty, correlation_id,
                                               memory_order_acq_rel, memory_order_acquire))
    return SALTS_EBUSY;
  if (atomic_compare_exchange_strong_explicit(&exchange->state, &expected, active_state,
                                              memory_order_release, memory_order_acquire))
    return SALTS_OK;
  atomic_store_explicit(&exchange->correlation_id, 0u, memory_order_release);
  return SALTS_EBUSY;
}

int flowie_pattern_exchange_match(const flowie_pattern_exchange_t *exchange,
                                      flowie_pattern_exchange_state_t expected_state,
                                      uint64_t correlation_id) {
  if (!flow_pattern_exchange_valid(exchange) || !flow_pattern_exchange_active_state(expected_state) ||
      correlation_id == 0u)
    return SALTS_EINVAL;
  if (atomic_load_explicit(&exchange->state, memory_order_acquire) != (int)expected_state)
    return SALTS_EBUSY;
  return atomic_load_explicit(&exchange->correlation_id, memory_order_acquire) == correlation_id
             ? SALTS_OK
             : SALTS_EPROTO;
}

int flowie_pattern_exchange_finish(flowie_pattern_exchange_t *exchange,
                                       flowie_pattern_exchange_state_t expected_state,
                                       uint64_t correlation_id,
                                       flowie_pattern_exchange_state_t terminal_state) {
  int rc;
  if (terminal_state != FLOWIE_PATTERN_EXCHANGE_READY &&
      terminal_state != FLOWIE_PATTERN_EXCHANGE_RESETTING)
    return SALTS_EINVAL;
  rc = flowie_pattern_exchange_match(exchange, expected_state, correlation_id);
  if (rc != SALTS_OK) return rc;
  if (terminal_state == FLOWIE_PATTERN_EXCHANGE_READY)
    atomic_store_explicit(&exchange->correlation_id, 0u, memory_order_release);
  atomic_store_explicit(&exchange->state, terminal_state, memory_order_release);
  return SALTS_OK;
}

int flowie_pattern_exchange_mark_resetting(flowie_pattern_exchange_t *exchange) {
  int state;
  if (!flow_pattern_exchange_valid(exchange)) return SALTS_EINVAL;
  state = atomic_load_explicit(&exchange->state, memory_order_acquire);
  if (!flow_pattern_exchange_active_state((flowie_pattern_exchange_state_t)state) &&
      state != FLOWIE_PATTERN_EXCHANGE_RESETTING)
    return SALTS_EBUSY;
  atomic_store_explicit(&exchange->state, FLOWIE_PATTERN_EXCHANGE_RESETTING,
                        memory_order_release);
  return SALTS_OK;
}

int flowie_pattern_exchange_reset(flowie_pattern_exchange_t *exchange) {
  if (!flow_pattern_exchange_valid(exchange)) return SALTS_EINVAL;
  atomic_store_explicit(&exchange->correlation_id, 0u, memory_order_release);
  atomic_store_explicit(&exchange->state, FLOWIE_PATTERN_EXCHANGE_READY, memory_order_release);
  return SALTS_OK;
}

int flowie_protocol_message_validate(const flowie_protocol_message_t *message) {
  if (!message || message->size < sizeof(*message) ||
      message->contract_version != FLOWIE_PROTOCOL_CONTRACT_VERSION ||
      message->protocol != FLOWIE_PROTOCOL_MQTT ||
      message->protocol_version == 0u || message->kind < FLOWIE_PROTOCOL_MESSAGE_DATA ||
      message->kind > FLOWIE_PROTOCOL_MESSAGE_ACK || message->duplicate > 1u ||
      message->retain > 1u || message->session_generation == 0u) {
    return SALTS_EINVAL;
  }
  if ((message->protocol_version != FLOWIE_MQTT_PROTOCOL_3_1 &&
       message->protocol_version != FLOWIE_MQTT_PROTOCOL_3_1_1 &&
       message->protocol_version != FLOWIE_MQTT_PROTOCOL_5_0) ||
      message->qos > FLOWIE_PROTOCOL_QOS_2 ||
      (message->qos == FLOWIE_PROTOCOL_QOS_0 && message->packet_id != 0u) ||
      (message->qos > FLOWIE_PROTOCOL_QOS_0 && message->packet_id == 0u)) {
    return SALTS_EPROTO;
  }
  return SALTS_OK;
}

int flowie_protocol_origin_validate(const flowie_protocol_origin_t *origin) {
  if (!origin || origin->size < sizeof(*origin) ||
      origin->contract_version != FLOWIE_PROTOCOL_CONTRACT_VERSION ||
      origin->protocol != FLOWIE_PROTOCOL_MQTT ||
      origin->protocol_version == 0u || origin->session_id == 0u) {
    return SALTS_EINVAL;
  }
  if (origin->protocol_version != FLOWIE_MQTT_PROTOCOL_3_1 &&
      origin->protocol_version != FLOWIE_MQTT_PROTOCOL_3_1_1 &&
      origin->protocol_version != FLOWIE_MQTT_PROTOCOL_5_0) {
    return SALTS_EPROTO;
  }
  return SALTS_OK;
}

int flowie_protocol_settlement_policy_validate(
    const flowie_protocol_settlement_policy_t *policy) {
  if (!policy || policy->size < sizeof(*policy) ||
      !flow_protocol_settlement_point_valid(policy->qos0) ||
      !flow_protocol_settlement_point_valid(policy->qos1) ||
      !flow_protocol_settlement_point_valid(policy->qos2)) {
    return SALTS_EINVAL;
  }
  return SALTS_OK;
}

int flowie_protocol_owner_settle(
    const flowie_protocol_owner_ops_t *owner, void *ctx,
    const flowie_protocol_settlement_policy_t *policy,
    const flowie_protocol_settlement_request_t *request) {
  flowie_protocol_settlement_point_t required;
  int rc;
  if (!owner || owner->size < sizeof(*owner) || !owner->settle || !request ||
      request->size < sizeof(*request) || request->attempt == 0u ||
      !flow_protocol_settlement_point_valid(request->point)) {
    return SALTS_EINVAL;
  }
  rc = flowie_protocol_message_validate(&request->message);
  if (rc != SALTS_OK) return rc;
  rc = flowie_protocol_settlement_policy_validate(policy);
  if (rc != SALTS_OK) return rc;
  required = request->message.qos == FLOWIE_PROTOCOL_QOS_0 ? policy->qos0
             : request->message.qos == FLOWIE_PROTOCOL_QOS_1 ? policy->qos1
                                                                 : policy->qos2;
  if (request->point != required) return SALTS_EBUSY;
  return owner->settle(ctx, request);
}
