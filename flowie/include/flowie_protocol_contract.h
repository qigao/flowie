#ifndef FLOWIE_PROTOCOL_H
#define FLOWIE_PROTOCOL_H

#include "platform.h"
#include "turbo_error.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_PROTOCOL_CONTRACT_VERSION 1u

typedef enum flowie_protocol_id_e {
  FLOWIE_PROTOCOL_MQTT = 1
} flowie_protocol_id_t;

/**
 * Protocol-neutral communication roles.
 *
 * A protocol adapter maps its wire-level roles onto this enum. The pattern
 * core owns only compatibility, candidate ordering, route fencing, and
 * synchronous correlation state; it never owns peers, payloads, queues, I/O,
 * topic matching, or acknowledgements.
 */
typedef enum flowie_pattern_role_e {
  FLOWIE_PATTERN_PUBLISH = 1,
  FLOWIE_PATTERN_SUBSCRIBE,
  FLOWIE_PATTERN_PUSH,
  FLOWIE_PATTERN_PULL,
  FLOWIE_PATTERN_ROUTE,
  FLOWIE_PATTERN_DEAL,
  FLOWIE_PATTERN_PAIR,
  FLOWIE_PATTERN_REQUEST,
  FLOWIE_PATTERN_REPLY,
  FLOWIE_PATTERN_EXTENDED_PUBLISH,
  FLOWIE_PATTERN_EXTENDED_SUBSCRIBE
} flowie_pattern_role_t;

typedef enum flowie_pattern_selection_e {
  FLOWIE_PATTERN_SELECT_FAN_OUT = 1,
  FLOWIE_PATTERN_SELECT_ROUND_ROBIN
} flowie_pattern_selection_t;

/** Host-owned selector cursor; it never owns the candidate set. */
typedef struct flowie_pattern_selector_s {
  size_t size;
  uint32_t contract_version;
  atomic_uint_fast64_t cursor;
} flowie_pattern_selector_t;

typedef struct flowie_pattern_selection_iterator_s {
  size_t size;
  uint32_t contract_version;
  flowie_pattern_selection_t selection;
  size_t candidate_count;
  size_t next_offset;
  size_t remaining;
} flowie_pattern_selection_iterator_t;

#define FLOWIE_PATTERN_SELECTION_ITERATOR_INIT                                               \
  {sizeof(flowie_pattern_selection_iterator_t), FLOWIE_PROTOCOL_CONTRACT_VERSION,        \
   (flowie_pattern_selection_t)0, 0u, 0u, 0u}

/** Pointer-free route association copied independently from a protocol wire frame. */
typedef struct flowie_pattern_route_s {
  size_t size;
  uint32_t contract_version;
  uint64_t route_id;
  uint64_t generation;
} flowie_pattern_route_t;

#define FLOWIE_PATTERN_ROUTE_INIT                                                            \
  {sizeof(flowie_pattern_route_t), FLOWIE_PROTOCOL_CONTRACT_VERSION, 0u, 0u}

typedef enum flowie_pattern_exchange_state_e {
  FLOWIE_PATTERN_EXCHANGE_READY = 0,
  FLOWIE_PATTERN_EXCHANGE_WAIT_REPLY,
  FLOWIE_PATTERN_EXCHANGE_PROCESSING_REQUEST,
  FLOWIE_PATTERN_EXCHANGE_RESETTING
} flowie_pattern_exchange_state_t;

/**
 * Host-owned synchronous request/reply association.
 *
 * The correlation is installed before the state becomes active and cleared
 * before READY is published. RESETTING deliberately retains the correlation
 * for diagnostics until the owning session calls reset.
 */
typedef struct flowie_pattern_exchange_s {
  size_t size;
  uint32_t contract_version;
  atomic_int state;
  atomic_uint_fast64_t correlation_id;
} flowie_pattern_exchange_t;

typedef enum flowie_mqtt_protocol_version_e {
  FLOWIE_MQTT_PROTOCOL_3_1 = 3,
  FLOWIE_MQTT_PROTOCOL_3_1_1 = 4,
  FLOWIE_MQTT_PROTOCOL_5_0 = 5
} flowie_mqtt_protocol_version_t;

typedef enum flowie_protocol_qos_e {
  FLOWIE_PROTOCOL_QOS_0 = 0,
  FLOWIE_PROTOCOL_QOS_1,
  FLOWIE_PROTOCOL_QOS_2
} flowie_protocol_qos_t;

typedef enum flowie_protocol_message_kind_e {
  FLOWIE_PROTOCOL_MESSAGE_DATA = 1,
  FLOWIE_PROTOCOL_MESSAGE_CONTROL,
  FLOWIE_PROTOCOL_MESSAGE_ACK
} flowie_protocol_message_kind_t;

typedef enum flowie_protocol_settlement_point_e {
  /** Protocol owner accepted the decoded packet before application admission. */
  FLOWIE_PROTOCOL_SETTLE_RECEIVED = 1,
  /** The configured application callback accepted ownership of the message. */
  FLOWIE_PROTOCOL_SETTLE_ACCEPTED,
  /** Application processing completed. */
  FLOWIE_PROTOCOL_SETTLE_PROCESSED,
  /** An explicit durable owner committed the message. */
  FLOWIE_PROTOCOL_SETTLE_DURABLE
} flowie_protocol_settlement_point_t;

/** Pointer-free MQTT metadata copied across Flowie owner boundaries. */
typedef struct flowie_protocol_message_s {
  size_t size;
  uint32_t contract_version;
  flowie_protocol_id_t protocol;
  uint32_t protocol_version;
  flowie_protocol_message_kind_t kind;
  uint32_t qos;
  uint32_t packet_id;
  uint64_t session_generation;
  uint8_t duplicate;
  uint8_t retain;
} flowie_protocol_message_t;

#define FLOWIE_PROTOCOL_MESSAGE_INIT                                                          \
  {sizeof(flowie_protocol_message_t), FLOWIE_PROTOCOL_CONTRACT_VERSION, 0, 0u, 0, 0u,    \
   0u, 0u, 0u, 0u}

/**
 * Process-local, pointer-free route copied with an owned flow message.
 *
 * A route identifies one live protocol-owner instance and one of its sessions.
 * It is intentionally not a durable or wire-format identity: serialization and
 * process restart must reject or discard it explicitly instead of replaying it.
 */
typedef struct flowie_protocol_route_s {
  size_t size;
  uint32_t contract_version;
  flowie_protocol_id_t protocol;
  uint32_t reserved;
  uint64_t owner_instance_id;
  uint64_t session_id;
  uint64_t session_generation;
} flowie_protocol_route_t;

#define FLOWIE_PROTOCOL_ROUTE_INIT                                                            \
  {sizeof(flowie_protocol_route_t), FLOWIE_PROTOCOL_CONTRACT_VERSION, 0, 0u, 0u, 0u, 0u}

/**
 * Serializable protocol origin retained across a durable queue boundary.
 *
 * Unlike a route, this value carries no live owner capability. `session_id` is
 * the protocol owner's stable publisher identity and may only be used for
 * message semantics such as MQTT no-local filtering.
 */
typedef struct flowie_protocol_origin_s {
  size_t size;
  uint32_t contract_version;
  flowie_protocol_id_t protocol;
  uint32_t protocol_version;
  uint64_t session_id;
} flowie_protocol_origin_t;

#define FLOWIE_PROTOCOL_ORIGIN_INIT                                                           \
  {sizeof(flowie_protocol_origin_t), FLOWIE_PROTOCOL_CONTRACT_VERSION, 0, 0u, 0u}

typedef struct flowie_protocol_settlement_policy_s {
  size_t size;
  flowie_protocol_settlement_point_t qos0;
  flowie_protocol_settlement_point_t qos1;
  flowie_protocol_settlement_point_t qos2;
} flowie_protocol_settlement_policy_t;

#define FLOWIE_PROTOCOL_SETTLEMENT_POLICY_INIT                                                \
  {sizeof(flowie_protocol_settlement_policy_t), FLOWIE_PROTOCOL_SETTLE_RECEIVED,          \
   FLOWIE_PROTOCOL_SETTLE_RECEIVED, FLOWIE_PROTOCOL_SETTLE_RECEIVED}

typedef struct flowie_protocol_settlement_request_s {
  size_t size;
  flowie_protocol_message_t message;
  flowie_protocol_settlement_point_t point;
  int status;
  uint64_t message_id;
  uint32_t attempt;
} flowie_protocol_settlement_request_t;

#define FLOWIE_PROTOCOL_SETTLEMENT_REQUEST_INIT                                               \
  {sizeof(flowie_protocol_settlement_request_t), FLOWIE_PROTOCOL_MESSAGE_INIT,            \
   FLOWIE_PROTOCOL_SETTLE_RECEIVED, TURBO_OK, 0u, 0u}

/**
 * Message-owned request for one later primitive settlement boundary.
 *
 * The envelope is pointer-free and process-local. `settled_point` is zero
 * until exactly one primitive accepts the requested responsibility.
 */
typedef struct flowie_protocol_settlement_envelope_s {
  size_t size;
  uint32_t contract_version;
  flowie_protocol_message_t message;
  flowie_protocol_settlement_point_t requested_point;
  flowie_protocol_settlement_point_t settled_point;
} flowie_protocol_settlement_envelope_t;

#define FLOWIE_PROTOCOL_SETTLEMENT_ENVELOPE_INIT                                              \
  {sizeof(flowie_protocol_settlement_envelope_t), FLOWIE_PROTOCOL_CONTRACT_VERSION,       \
   FLOWIE_PROTOCOL_MESSAGE_INIT, FLOWIE_PROTOCOL_SETTLE_RECEIVED,                         \
   (flowie_protocol_settlement_point_t)0}

typedef int (*flowie_protocol_settle_fn)(
    void *ctx, const flowie_protocol_settlement_request_t *request);

typedef struct flowie_protocol_owner_ops_s {
  size_t size;
  flowie_protocol_settle_fn settle;
} flowie_protocol_owner_ops_t;

#define FLOWIE_PROTOCOL_OWNER_OPS_INIT {sizeof(flowie_protocol_owner_ops_t), NULL}

CXX_C_API int flowie_protocol_message_validate(
    const flowie_protocol_message_t *message);
CXX_C_API int flowie_protocol_origin_validate(const flowie_protocol_origin_t *origin);
CXX_C_API int flowie_pattern_role_validate(flowie_pattern_role_t role);
CXX_C_API int flowie_pattern_roles_compatible(flowie_pattern_role_t local,
                                                   flowie_pattern_role_t remote);
CXX_C_API int flowie_pattern_selector_init(flowie_pattern_selector_t *selector);
CXX_C_API int flowie_pattern_selection_begin(
    flowie_pattern_selector_t *selector, flowie_pattern_selection_t selection,
    size_t candidate_count, flowie_pattern_selection_iterator_t *iterator);
/** Returns TURBO_ENOENT after every candidate has been produced exactly once. */
CXX_C_API int flowie_pattern_selection_next(
    flowie_pattern_selection_iterator_t *iterator, size_t *candidate_index);
CXX_C_API int flowie_pattern_route_validate(const flowie_pattern_route_t *route);
CXX_C_API int flowie_pattern_routes_match(const flowie_pattern_route_t *requested,
                                               const flowie_pattern_route_t *candidate);
CXX_C_API int flowie_pattern_exchange_init(flowie_pattern_exchange_t *exchange);
CXX_C_API int flowie_pattern_exchange_snapshot(
    const flowie_pattern_exchange_t *exchange, flowie_pattern_exchange_state_t *state,
    uint64_t *correlation_id);
CXX_C_API int flowie_pattern_exchange_begin(
    flowie_pattern_exchange_t *exchange, flowie_pattern_exchange_state_t active_state,
    uint64_t correlation_id);
CXX_C_API int flowie_pattern_exchange_match(
    const flowie_pattern_exchange_t *exchange,
    flowie_pattern_exchange_state_t expected_state, uint64_t correlation_id);
CXX_C_API int flowie_pattern_exchange_finish(
    flowie_pattern_exchange_t *exchange,
    flowie_pattern_exchange_state_t expected_state, uint64_t correlation_id,
    flowie_pattern_exchange_state_t terminal_state);
CXX_C_API int flowie_pattern_exchange_mark_resetting(
    flowie_pattern_exchange_t *exchange);
CXX_C_API int flowie_pattern_exchange_reset(flowie_pattern_exchange_t *exchange);
CXX_C_API int flowie_protocol_settlement_policy_validate(
    const flowie_protocol_settlement_policy_t *policy);
/** Dispatch exactly one decision to the protocol owner; this function never generates an ACK. */
CXX_C_API int flowie_protocol_owner_settle(
    const flowie_protocol_owner_ops_t *owner, void *ctx,
    const flowie_protocol_settlement_policy_t *policy,
    const flowie_protocol_settlement_request_t *request);

#ifdef __cplusplus
}
#endif

#endif
