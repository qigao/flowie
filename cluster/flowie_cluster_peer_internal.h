#ifndef FLOWIE_CLUSTER_PEER_INTERNAL_H
#define FLOWIE_CLUSTER_PEER_INTERNAL_H

#include "flowie_cluster_internal.h"

#include "salts_error.h"
#include "flowie_security.h"
#include "salts_str.h"
#include <cnet/cnet.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_CLUSTER_PEER_WIRE_VERSION 1u
#define FLOWIE_CLUSTER_PEER_HEADER_SIZE 112u
#define FLOWIE_CLUSTER_PEER_CORRELATION_ID_SIZE 16u
#define FLOWIE_CLUSTER_PEER_INCOMPLETE 1
#define FLOWIE_CLUSTER_PEER_TRANSPORT_ABI_V1 1u
#define FLOWIE_CLUSTER_PEER_OWNER_ABI_V1 1u
#define FLOWIE_CLUSTER_PEER_CONNECT_BIND_VERSION 2u
#define FLOWIE_CLUSTER_PEER_CONNECT_BIND_HEADER_SIZE 32u
#define FLOWIE_CLUSTER_PEER_ADDRESS_MAX 127u
#define FLOWIE_CLUSTER_PEER_MQTT_COMMAND_VERSION 1u
#define FLOWIE_CLUSTER_PEER_MQTT_COMMAND_HEADER_SIZE 20u
#define FLOWIE_CLUSTER_PEER_MQTT_REPLY_VERSION 1u
#define FLOWIE_CLUSTER_PEER_MQTT_REPLY_HEADER_SIZE 20u
#define FLOWIE_CLUSTER_PEER_CONNECTION_LOST_VERSION 1u
#define FLOWIE_CLUSTER_PEER_CONNECTION_LOST_HEADER_SIZE 16u
#define FLOWIE_CLUSTER_PEER_TAKEOVER_CLOSE_VERSION 1u
#define FLOWIE_CLUSTER_PEER_TAKEOVER_CLOSE_HEADER_SIZE 16u
#define FLOWIE_CLUSTER_PEER_EDGE_ACTION_VERSION 1u
#define FLOWIE_CLUSTER_PEER_EDGE_ACTION_HEADER_SIZE 24u
#define FLOWIE_CLUSTER_PEER_EDGE_ACTION_ACK_VERSION 1u
#define FLOWIE_CLUSTER_PEER_EDGE_ACTION_ACK_SIZE 24u
#define FLOWIE_CLUSTER_PEER_PUBLISH_SETTLE_VERSION 1u
#define FLOWIE_CLUSTER_PEER_PUBLISH_SETTLE_HEADER_SIZE 64u

typedef enum flowie_cluster_peer_frame_kind_e {
  FLOWIE_CLUSTER_PEER_FRAME_HELLO = 1,
  FLOWIE_CLUSTER_PEER_FRAME_HELLO_ACK,
  FLOWIE_CLUSTER_PEER_FRAME_COMMAND,
  FLOWIE_CLUSTER_PEER_FRAME_REPLY,
  FLOWIE_CLUSTER_PEER_FRAME_EVENT,
  FLOWIE_CLUSTER_PEER_FRAME_PING,
  FLOWIE_CLUSTER_PEER_FRAME_PONG,
  FLOWIE_CLUSTER_PEER_FRAME_GOAWAY
} flowie_cluster_peer_frame_kind_t;

typedef enum flowie_cluster_peer_operation_e {
  FLOWIE_CLUSTER_PEER_OPERATION_NONE = 0,
  FLOWIE_CLUSTER_PEER_OPERATION_CONNECT_BIND = 1,
  FLOWIE_CLUSTER_PEER_OPERATION_MQTT_PUBLISH,
  FLOWIE_CLUSTER_PEER_OPERATION_MQTT_SUBSCRIBE,
  FLOWIE_CLUSTER_PEER_OPERATION_MQTT_UNSUBSCRIBE,
  FLOWIE_CLUSTER_PEER_OPERATION_MQTT_ACK,
  FLOWIE_CLUSTER_PEER_OPERATION_MQTT_DISCONNECT,
  FLOWIE_CLUSTER_PEER_OPERATION_CONNECTION_LOST,
  FLOWIE_CLUSTER_PEER_OPERATION_TAKEOVER_CLOSE,
  FLOWIE_CLUSTER_PEER_OPERATION_MQTT_REPLY,
  FLOWIE_CLUSTER_PEER_OPERATION_EVENT_DELIVER,
  FLOWIE_CLUSTER_PEER_OPERATION_EVENT_ACK,
  FLOWIE_CLUSTER_PEER_OPERATION_EDGE_ACTION,
  FLOWIE_CLUSTER_PEER_OPERATION_EDGE_ACTION_ACK,
  FLOWIE_CLUSTER_PEER_OPERATION_MQTT_PUBLISH_SETTLE
} flowie_cluster_peer_operation_t;

/**
 * One internal peer frame. Encode borrows all views. Decode owns one
 * contiguous copy in storage and exposes views into it until cleanup.
 */
typedef struct flowie_cluster_peer_frame_s {
  size_t size;
  flowie_cluster_peer_frame_kind_t kind;
  flowie_cluster_peer_operation_t operation;
  uint32_t flags;
  uint32_t shard_id;
  int32_t status;
  uint64_t owner_epoch;
  uint64_t connection_id;
  uint64_t connection_generation;
  uint8_t source_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE];
  uint8_t target_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE];
  uint8_t correlation_id[FLOWIE_CLUSTER_PEER_CORRELATION_ID_SIZE];
  vstr cluster_id;
  vstr listener_id;
  vstr source_node_id;
  vstr target_node_id;
  vstr payload;
  tstr storage;
} flowie_cluster_peer_frame_t;

#define FLOWIE_CLUSTER_PEER_FRAME_INIT                                                             \
  {sizeof(flowie_cluster_peer_frame_t),                                                            \
   FLOWIE_CLUSTER_PEER_FRAME_HELLO,                                                                \
   FLOWIE_CLUSTER_PEER_OPERATION_NONE,                                                             \
   0u,                                                                                             \
   0u,                                                                                             \
   0,                                                                                              \
   0u,                                                                                             \
   0u,                                                                                             \
   0u,                                                                                             \
   {0},                                                                                            \
   {0},                                                                                            \
   {0},                                                                                            \
   {NULL, 0u},                                                                                     \
   {NULL, 0u},                                                                                     \
   {NULL, 0u},                                                                                     \
   {NULL, 0u},                                                                                     \
   {NULL, 0u},                                                                                     \
   NULL}

/** Validate semantics and calculate the exact encoded byte count. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_frame_encoded_size(const flowie_cluster_peer_frame_t *frame,
                                                     size_t max_payload_size, size_t *out_size);

/** Encode one complete frame. Caller releases *out with tstr_free(). */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_frame_encode(const flowie_cluster_peer_frame_t *frame,
                                               size_t max_payload_size, tstr *out);

/**
 * Decode the first complete frame. Incomplete input is not consumed. On
 * success, caller releases owned storage with frame_cleanup().
 */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_frame_decode(const void *data, size_t data_size,
                                               size_t max_payload_size,
                                               flowie_cluster_peer_frame_t *out, size_t *consumed);

/**
 * Require the decoded route to target this exact cluster and process
 * incarnation. A mismatch is a protocol error, never a routing fallback.
 */
FLOWIE_INTERNAL_C_API int
flowie_cluster_peer_frame_require_target(const flowie_cluster_peer_frame_t *frame,
                                         vstr cluster_id, vstr target_node_id,
                                         const uint8_t target_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE]);

/** Release decode-owned storage and reset the frame. NULL is accepted. */
FLOWIE_INTERNAL_C_API void flowie_cluster_peer_frame_cleanup(flowie_cluster_peer_frame_t *frame);

/**
 * Authenticated CONNECT state safe to transfer to a shard owner. The parsed
 * CONNECT borrows the encoded payload; credentials and authentication
 * properties are never present. The principal is copied by value.
 */
typedef struct flowie_cluster_peer_connect_bind_view_s {
  size_t size;
  uint32_t abi_version;
  uint8_t security_enabled;
  flowie_security_principal_t principal;
  flowie_mqtt_packet_view_t packet;
  flowie_mqtt_connect_view_t connect;
  /** Advisory edge observations; never ownership or fencing authority. */
  vstr remote_address;
  vstr transport_peer_address;
  vstr proxy_tlvs;
} flowie_cluster_peer_connect_bind_view_t;

#define FLOWIE_CLUSTER_PEER_CONNECT_BIND_VIEW_INIT                                                 \
  {sizeof(flowie_cluster_peer_connect_bind_view_t),                                                \
   FLOWIE_CLUSTER_PEER_CONNECT_BIND_VERSION,                                                       \
   0u,                                                                                             \
   FLOWIE_SECURITY_PRINCIPAL_INIT,                                                             \
   FLOWIE_MQTT_PACKET_VIEW_INIT,                                                                   \
   FLOWIE_MQTT_CONNECT_VIEW_INIT,                                                                  \
   {NULL, 0u},                                                                                     \
   {NULL, 0u},                                                                                     \
   {NULL, 0u}}

typedef struct flowie_cluster_peer_ingress_metadata_s {
  vstr remote_address;
  vstr transport_peer_address;
  vstr proxy_tlvs;
} flowie_cluster_peer_ingress_metadata_t;

#define FLOWIE_CLUSTER_PEER_INGRESS_METADATA_INIT {{NULL, 0u}, {NULL, 0u}, {NULL, 0u}}

/**
 * Encode one parser-validated CONNECT after authentication. A non-NULL
 * principal enables security and is serialized; username, password,
 * authentication method/data and edge-only CONNECT properties are omitted.
 * Caller releases *out with tstr_free().
 */
FLOWIE_INTERNAL_C_API int
flowie_cluster_peer_connect_bind_encode(const flowie_mqtt_connect_view_t *connect,
                                        const flowie_security_principal_t *principal,
                                        size_t max_payload_size, tstr *out);

/** Encode CONNECT plus bounded advisory edge observations. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_connect_bind_encode_with_metadata(
    const flowie_mqtt_connect_view_t *connect,
    const flowie_security_principal_t *principal,
    const flowie_cluster_peer_ingress_metadata_t *metadata, size_t max_payload_size,
    tstr *out);

/** Decode one borrowed CONNECT_BIND payload and revalidate all wire invariants. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_connect_bind_decode(const void *data, size_t data_size,
                                                      size_t max_payload_size,
                                                      flowie_cluster_peer_connect_bind_view_t *out);

/**
 * Borrowed, validated post-CONNECT MQTT command. The client ID and parsed
 * packet views remain valid only while the encoded payload remains unchanged.
 */
typedef struct flowie_cluster_peer_mqtt_command_view_s {
  size_t size;
  uint32_t abi_version;
  flowie_cluster_peer_operation_t operation;
  flowie_mqtt_version_t mqtt_version;
  flowie_mqtt_span_t client_id;
  flowie_mqtt_packet_view_t packet;
} flowie_cluster_peer_mqtt_command_view_t;

#define FLOWIE_CLUSTER_PEER_MQTT_COMMAND_VIEW_INIT                                                 \
  {sizeof(flowie_cluster_peer_mqtt_command_view_t),                                                \
   FLOWIE_CLUSTER_PEER_MQTT_COMMAND_VERSION,                                                       \
   FLOWIE_CLUSTER_PEER_OPERATION_NONE,                                                             \
   FLOWIE_MQTT_VERSION_UNSPECIFIED,                                                                \
   {NULL, 0u},                                                                                     \
   FLOWIE_MQTT_PACKET_VIEW_INIT}

/**
 * Encode one post-CONNECT packet command. CONNECT_BIND, CONNECTION_LOST and
 * TAKEOVER_CLOSE use separate contracts and are rejected here.
 */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_mqtt_command_encode(flowie_cluster_peer_operation_t operation,
                                                      flowie_mqtt_version_t mqtt_version,
                                                      flowie_mqtt_span_t client_id,
                                                      flowie_mqtt_span_t packet,
                                                      size_t max_packet_size, tstr *out);

/** Decode and fully validate one borrowed command payload and its typed MQTT packet body. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_mqtt_command_decode(flowie_cluster_peer_operation_t operation,
                                                      const void *data, size_t data_size,
                                                      size_t max_packet_size,
                                                      flowie_cluster_peer_mqtt_command_view_t *out);

/** Borrowed Client ID plus a pointer-free graph settlement for one inbound PUBLISH. */
typedef struct flowie_cluster_peer_publish_settle_view_s {
  size_t size;
  uint32_t abi_version;
  flowie_mqtt_span_t client_id;
  flowie_protocol_settlement_request_t settlement;
} flowie_cluster_peer_publish_settle_view_t;

#define FLOWIE_CLUSTER_PEER_PUBLISH_SETTLE_VIEW_INIT                                              \
  {sizeof(flowie_cluster_peer_publish_settle_view_t),                                             \
   FLOWIE_CLUSTER_PEER_PUBLISH_SETTLE_VERSION,                                                    \
   {NULL, 0u},                                                                                    \
   FLOWIE_PROTOCOL_SETTLEMENT_REQUEST_INIT}

/** Encode one graph completion for validation and durable mutation by the session owner. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_publish_settle_encode(
    flowie_mqtt_span_t client_id,
    const flowie_protocol_settlement_request_t *settlement, size_t max_payload_size,
    tstr *out);

/** Decode a borrowed PUBLISH_SETTLE payload and revalidate every serialized invariant. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_publish_settle_decode(
    const void *data, size_t data_size, size_t max_payload_size,
    flowie_cluster_peer_publish_settle_view_t *out);

/**
 * Borrowed, validated socket action returned by the session owner. Settlement
 * reports an owner-completed boundary; it never authorizes edge-local session
 * mutation. A payload with no packet, close or settlement is an explicit no-op.
 */
typedef struct flowie_cluster_peer_mqtt_reply_action_s {
  size_t size;
  uint32_t abi_version;
  flowie_mqtt_version_t mqtt_version;
  uint8_t close_after_send;
  flowie_protocol_settlement_point_t settlement_point;
  flowie_mqtt_packet_view_t packet;
} flowie_cluster_peer_mqtt_reply_action_t;

#define FLOWIE_CLUSTER_PEER_MQTT_REPLY_ACTION_INIT                                                 \
  {sizeof(flowie_cluster_peer_mqtt_reply_action_t),                                                \
   FLOWIE_CLUSTER_PEER_MQTT_REPLY_VERSION,                                                         \
   FLOWIE_MQTT_VERSION_UNSPECIFIED,                                                                \
   0u,                                                                                             \
   (flowie_protocol_settlement_point_t)0,                                                      \
   FLOWIE_MQTT_PACKET_VIEW_INIT}

/** Encode one owner result. packet may be empty for a close, settlement or no-op action. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_mqtt_reply_encode(
    flowie_mqtt_version_t mqtt_version, flowie_mqtt_span_t packet, int close_after_send,
    flowie_protocol_settlement_point_t settlement_point, size_t max_packet_size, tstr *out);

/** Decode and fully validate one borrowed owner result and optional outbound MQTT packet. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_mqtt_reply_decode(const void *data, size_t data_size,
                                                    size_t max_packet_size,
                                                    flowie_cluster_peer_mqtt_reply_action_t *out);

/** One owner-issued, connection-ordered socket action independent of a request reply. */
typedef struct flowie_cluster_peer_edge_action_s {
  size_t size;
  uint32_t abi_version;
  uint64_t action_sequence;
  flowie_cluster_peer_mqtt_reply_action_t action;
} flowie_cluster_peer_edge_action_t;

#define FLOWIE_CLUSTER_PEER_EDGE_ACTION_INIT                                                      \
  {sizeof(flowie_cluster_peer_edge_action_t), FLOWIE_CLUSTER_PEER_EDGE_ACTION_VERSION, 0u,        \
   FLOWIE_CLUSTER_PEER_MQTT_REPLY_ACTION_INIT}

/** Encode a TFRP action inside a sequenced TFEA command payload. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_edge_action_encode(
    uint64_t action_sequence, flowie_mqtt_version_t mqtt_version, flowie_mqtt_span_t packet,
    int close_after_send, flowie_protocol_settlement_point_t settlement_point,
    size_t max_payload_size, tstr *out);
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_edge_action_decode(const void *data, size_t data_size,
                                                     size_t max_payload_size,
                                                     flowie_cluster_peer_edge_action_t *out);

/** EDGE_ACTION_ACK echoes the exact applied or replayed sequence. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_edge_action_ack_encode(uint64_t action_sequence, tstr *out);
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_edge_action_ack_decode(const void *data, size_t data_size,
                                                         uint64_t *out_action_sequence);

/** Borrowed, validated identity carried by a CONNECTION_LOST lifecycle command. */
typedef struct flowie_cluster_peer_connection_lost_view_s {
  size_t size;
  uint32_t abi_version;
  flowie_mqtt_version_t mqtt_version;
  flowie_mqtt_span_t client_id;
} flowie_cluster_peer_connection_lost_view_t;

#define FLOWIE_CLUSTER_PEER_CONNECTION_LOST_VIEW_INIT                                              \
  {sizeof(flowie_cluster_peer_connection_lost_view_t),                                             \
   FLOWIE_CLUSTER_PEER_CONNECTION_LOST_VERSION,                                                    \
   FLOWIE_MQTT_VERSION_UNSPECIFIED,                                                                \
   {NULL, 0u}}

/** Encode one abnormal edge-socket loss. Caller releases *out with tstr_free(). */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_connection_lost_encode(flowie_mqtt_version_t mqtt_version,
                                                         flowie_mqtt_span_t client_id,
                                                         size_t max_payload_size, tstr *out);

/** Decode one borrowed CONNECTION_LOST payload and revalidate its client identity. */
FLOWIE_INTERNAL_C_API int
flowie_cluster_peer_connection_lost_decode(const void *data, size_t data_size,
                                           size_t max_payload_size,
                                           flowie_cluster_peer_connection_lost_view_t *out);

/** Borrowed identity of the exact old edge socket generation to fence. */
typedef struct flowie_cluster_peer_takeover_close_view_s {
  size_t size;
  uint32_t abi_version;
  flowie_mqtt_version_t mqtt_version;
  flowie_mqtt_span_t client_id;
} flowie_cluster_peer_takeover_close_view_t;

#define FLOWIE_CLUSTER_PEER_TAKEOVER_CLOSE_VIEW_INIT                                               \
  {sizeof(flowie_cluster_peer_takeover_close_view_t),                                              \
   FLOWIE_CLUSTER_PEER_TAKEOVER_CLOSE_VERSION,                                                     \
   FLOWIE_MQTT_VERSION_UNSPECIFIED,                                                                \
   {NULL, 0u}}

/** Encode one owner-issued old-socket fence. Caller releases *out with tstr_free(). */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_takeover_close_encode(flowie_mqtt_version_t mqtt_version,
                                                        flowie_mqtt_span_t client_id,
                                                        size_t max_payload_size, tstr *out);

/** Decode one borrowed TAKEOVER_CLOSE payload and revalidate its client identity. */
FLOWIE_INTERNAL_C_API int
flowie_cluster_peer_takeover_close_decode(const void *data, size_t data_size,
                                          size_t max_payload_size,
                                          flowie_cluster_peer_takeover_close_view_t *out);

typedef enum flowie_cluster_peer_role_e {
  FLOWIE_CLUSTER_PEER_ROLE_INITIATOR = 1,
  FLOWIE_CLUSTER_PEER_ROLE_RESPONDER
} flowie_cluster_peer_role_t;

typedef enum flowie_cluster_peer_link_state_e {
  FLOWIE_CLUSTER_PEER_LINK_CREATED = 1,
  FLOWIE_CLUSTER_PEER_LINK_HANDSHAKING,
  FLOWIE_CLUSTER_PEER_LINK_ACTIVE,
  FLOWIE_CLUSTER_PEER_LINK_CLOSING,
  FLOWIE_CLUSTER_PEER_LINK_CLOSED
} flowie_cluster_peer_link_state_t;

typedef struct flowie_cluster_peer_link_s flowie_cluster_peer_link_t;

typedef int (*flowie_cluster_peer_authorize_fn)(
    void *user_data, vstr peer_node_id, const uint8_t peer_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE],
    const char *peer_certificate_sha256);
/**
 * Publish an authenticated link after its state becomes ACTIVE. The identity
 * views remain link-owned and are only valid until link_run() returns.
 */
typedef int (*flowie_cluster_peer_active_fn)(
    void *user_data, flowie_cluster_peer_link_t *link, vstr peer_node_id,
    const uint8_t peer_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE]);
typedef int (*flowie_cluster_peer_receive_fn)(void *user_data,
                                              const flowie_cluster_peer_frame_t *frame);
typedef void (*flowie_cluster_peer_send_complete_fn)(void *user_data, int status);

typedef struct flowie_cluster_peer_link_config_s {
  size_t size;
  uint32_t abi_version;
  flowie_cluster_peer_role_t role;
  size_t max_payload_size;
  size_t queue_entries;
  size_t queue_bytes;
  vstr cluster_id;
  vstr local_node_id;
  uint8_t local_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE];
  vstr remote_node_id;
  uint8_t remote_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE];
  flowie_cluster_peer_authorize_fn authorize;
  flowie_cluster_peer_active_fn active;
  flowie_cluster_peer_receive_fn receive;
  void *user_data;
} flowie_cluster_peer_link_config_t;

#define FLOWIE_CLUSTER_PEER_LINK_CONFIG_INIT                                                       \
  {sizeof(flowie_cluster_peer_link_config_t),                                                      \
   FLOWIE_CLUSTER_PEER_TRANSPORT_ABI_V1,                                                           \
   FLOWIE_CLUSTER_PEER_ROLE_INITIATOR,                                                             \
   0u,                                                                                             \
   0u,                                                                                             \
   0u,                                                                                             \
   {NULL, 0u},                                                                                     \
   {NULL, 0u},                                                                                     \
   {0},                                                                                            \
   {NULL, 0u},                                                                                     \
   {0},                                                                                            \
   NULL,                                                                                           \
   NULL,                                                                                           \
   NULL,                                                                                           \
   NULL}

/**
 * Create a bounded link state machine. No socket is opened and all identity
 * views are copied. The responder learns its remote identity only after the
 * authorization callback accepts the mTLS-bound HELLO.
 */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_link_create(const flowie_cluster_peer_link_config_t *config,
                                              flowie_cluster_peer_link_t **out);

/** Destroy only after run() returned; a live link returns SALTS_EBUSY. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_link_destroy(flowie_cluster_peer_link_t *link);

/**
 * Bind the link to a caller-owned CNet client and return the observer that must
 * be supplied when connecting or adopting the TLS connection. The caller must
 * then run the link on the same CNet owner thread.
 */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_link_observer(flowie_cluster_peer_link_t *link,
                                                cnet_client *network,
                                                cnet_observer *out_observer);

/**
 * Run with the TLS connection created using link_observer(). The connection
 * remains caller-owned. Verified peer certificate and the CNet TLS exporter
 * binding are mandatory before the Flowie HELLO exchange.
 */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_link_run(flowie_cluster_peer_link_t *link,
                                            cnet_connection connected_tls_connection);

/**
 * Thread-safe bounded admission. The frame is encoded and owned by the link
 * until completion; ENOSPC never queues a partial command.
 */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_link_send(flowie_cluster_peer_link_t *link,
                                            const flowie_cluster_peer_frame_t *frame,
                                            flowie_cluster_peer_send_complete_fn complete,
                                            void *complete_user_data);

/** Stop admission and wake the owner; already admitted sends are drained. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_link_close(flowie_cluster_peer_link_t *link);

FLOWIE_INTERNAL_C_API flowie_cluster_peer_link_state_t
flowie_cluster_peer_link_state(flowie_cluster_peer_link_t *link);
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_link_pending(flowie_cluster_peer_link_t *link, size_t *entries,
                                               size_t *bytes);

#define FLOWIE_CLUSTER_PEER_REGISTRY_ABI_V1 1u

typedef struct flowie_cluster_peer_registry_config_s {
  size_t size;
  uint32_t abi_version;
  size_t max_links;
  size_t max_inflight_sends;
} flowie_cluster_peer_registry_config_t;

#define FLOWIE_CLUSTER_PEER_REGISTRY_CONFIG_INIT                                                   \
  {sizeof(flowie_cluster_peer_registry_config_t), FLOWIE_CLUSTER_PEER_REGISTRY_ABI_V1, 0u, 0u}

typedef struct flowie_cluster_peer_registry_snapshot_s {
  size_t size;
  uint32_t abi_version;
  size_t registered_links;
  size_t draining_links;
  size_t inflight_sends;
  int closing;
} flowie_cluster_peer_registry_snapshot_t;

#define FLOWIE_CLUSTER_PEER_REGISTRY_SNAPSHOT_INIT                                                 \
  {sizeof(flowie_cluster_peer_registry_snapshot_t),                                                \
   FLOWIE_CLUSTER_PEER_REGISTRY_ABI_V1,                                                            \
   0u,                                                                                             \
   0u,                                                                                             \
   0u,                                                                                             \
   0}

typedef struct flowie_cluster_peer_registry_s flowie_cluster_peer_registry_t;

/**
 * Registry borrows links. An EBUSY unregister stops new sends and permits the
 * caller to close the link, but the caller must retry unregister successfully
 * before destroying it. Keep the registry alive through every accepted send
 * completion.
 */
FLOWIE_INTERNAL_C_API int
flowie_cluster_peer_registry_create(const flowie_cluster_peer_registry_config_t *config,
                                    flowie_cluster_peer_registry_t **out);
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_registry_register(
    flowie_cluster_peer_registry_t *registry, vstr remote_node_id,
    const uint8_t remote_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE], flowie_cluster_peer_link_t *link);
/** First call may return EBUSY while preventing new sends; retry after accepted sends complete. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_registry_unregister(
    flowie_cluster_peer_registry_t *registry, vstr remote_node_id,
    const uint8_t remote_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE], flowie_cluster_peer_link_t *link);
/** Dispatcher-compatible send adapter routed by the frame's exact target identity. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_registry_send(void *ctx, const flowie_cluster_peer_frame_t *frame,
                                                flowie_cluster_peer_send_complete_fn complete,
                                                void *complete_ctx);
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_registry_snapshot(flowie_cluster_peer_registry_t *registry,
                                                    flowie_cluster_peer_registry_snapshot_t *out);
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_registry_close(flowie_cluster_peer_registry_t *registry);
/** Wait for every accepted transport send and its completion callback to return. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_registry_drain(flowie_cluster_peer_registry_t *registry,
                                                 uint64_t timeout_ns);
/** Requires close(), drain(), and successful unregister of every borrowed link. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_registry_destroy(flowie_cluster_peer_registry_t *registry);

typedef struct flowie_cluster_peer_owner_s flowie_cluster_peer_owner_t;

/** Resolve the authority that is current on the owner lane at execution time. */
typedef int (*flowie_cluster_peer_owner_resolve_fn)(void *user_data, uint32_t shard_id,
                                                    flowie_cluster_owner_token_t *out);

/**
 * Apply one fenced command on the owner lane. The command is borrowed for the
 * call. A returned payload is owned by the adapter and released after reply().
 */
typedef int (*flowie_cluster_peer_owner_execute_fn)(void *user_data,
                                                    const flowie_cluster_peer_frame_t *command,
                                                    tstr *reply_payload);

/**
 * Publish or abort staged state after durable work completes. This callback is
 * invoked exactly once on the owner lane when complete() succeeds. It consumes
 * finalize_ctx and may transfer a prebuilt payload through reply_payload. It
 * must not allocate or perform blocking I/O: successful publication is the
 * no-failure pointer/state transition after the durable commit.
 */
typedef int (*flowie_cluster_peer_owner_finalize_fn)(void *finalize_ctx, int durable_status,
                                                     tstr *reply_payload);

/**
 * Finish one asynchronously accepted owner command. This schedules finalize()
 * and the derived reply back onto the owner lane. The completion context is
 * single-use. When this returns SALTS_OK, the adapter owns the finalize
 * obligation; when scheduling fails, finalize() is not called and the provider
 * retains finalize_ctx and must fail the shard closed because no cross-thread
 * MQTT-state mutation or reply callback is permitted.
 */
typedef int (*flowie_cluster_peer_owner_complete_fn)(void *completion_ctx, int durable_status,
                                                     flowie_cluster_peer_owner_finalize_fn finalize,
                                                     void *finalize_ctx);

/**
 * Stage one command on the owner lane and hand its durable work to an
 * asynchronous provider. Returning SALTS_OK transfers exactly one completion
 * obligation to the provider; any other status means completion must not be
 * called. Borrowed command views expire when this function returns. While one
 * asynchronous command is outstanding, later commands receive SALTS_EBUSY so
 * two fact revisions cannot advance concurrently on the same adapter.
 */
typedef int (*flowie_cluster_peer_owner_execute_async_fn)(
    void *user_data, const flowie_cluster_peer_frame_t *command,
    flowie_cluster_peer_owner_complete_fn complete, void *completion_ctx);

/**
 * Observe one derived reply on the owner lane. The frame and all its views are
 * borrowed for the call; link_send() may be used because it copies the frame.
 */
typedef int (*flowie_cluster_peer_owner_reply_fn)(void *user_data,
                                                  const flowie_cluster_peer_frame_t *reply);

typedef struct flowie_cluster_peer_owner_config_s {
  size_t size;
  uint32_t abi_version;
  size_t max_payload_size;
  size_t queue_entries;
  size_t queue_bytes;
  vstr cluster_id;
  vstr listener_id;
  vstr local_node_id;
  uint8_t local_boot_id[FLOWIE_CLUSTER_BOOT_ID_SIZE];
  flowie_cluster_peer_owner_resolve_fn resolve;
  flowie_cluster_peer_owner_execute_fn execute;
  flowie_cluster_peer_owner_execute_async_fn execute_async;
  flowie_cluster_peer_owner_reply_fn reply;
  void *user_data;
} flowie_cluster_peer_owner_config_t;

#define FLOWIE_CLUSTER_PEER_OWNER_CONFIG_INIT                                                      \
  {sizeof(flowie_cluster_peer_owner_config_t),                                                     \
   FLOWIE_CLUSTER_PEER_OWNER_ABI_V1,                                                               \
   0u,                                                                                             \
   0u,                                                                                             \
   0u,                                                                                             \
   {NULL, 0u},                                                                                     \
   {NULL, 0u},                                                                                     \
   {NULL, 0u},                                                                                     \
   {0},                                                                                            \
   NULL,                                                                                           \
   NULL,                                                                                           \
   NULL,                                                                                           \
   NULL,                                                                                           \
   NULL}

/** Create a bounded asynchronous adapter; all identity views are copied. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_owner_create(const flowie_cluster_peer_owner_config_t *config,
                                               flowie_cluster_peer_owner_t **out);

/**
 * Copy and admit one COMMAND. Accepted commands always resolve and fence on
 * the owner lane before execute(); overload is returned as SALTS_ENOSPC.
 */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_owner_submit(flowie_cluster_peer_owner_t *owner,
                                               const flowie_cluster_peer_frame_t *command);

/** Stop admission; already accepted commands still produce one reply each. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_owner_close(flowie_cluster_peer_owner_t *owner);

/** Wait until every accepted command and reply callback has completed. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_owner_drain(flowie_cluster_peer_owner_t *owner,
                                              uint64_t timeout_ns);

/** Destroy only after close and drain; otherwise returns SALTS_EBUSY. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_owner_destroy(flowie_cluster_peer_owner_t *owner);

/** Initialize fail-closed reusable CNet mTLS profiles for cluster links. */
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_tls_client_configure(
    cnet_tls_client *client, const char *ca_file, const char *cert_file,
    const char *key_file, const char *key_password, const char *server_name);
FLOWIE_INTERNAL_C_API int flowie_cluster_peer_tls_server_configure(
    cnet_tls_server *server, const char *ca_file, const char *cert_file,
    const char *key_file, const char *key_password);

#ifdef __cplusplus
}
#endif

#endif /* FLOWIE_CLUSTER_PEER_INTERNAL_H */
