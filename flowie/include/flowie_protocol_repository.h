#ifndef FLOWIE_PROTOCOL_REPOSITORY_H
#define FLOWIE_PROTOCOL_REPOSITORY_H

#include "flowie_export.h"
#include "flowie_mqtt_protocol.h"
#include "flowie_security.h"
#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_PROTOCOL_REPOSITORY_SCHEMA_VERSION 2u

typedef struct flowie_protocol_repository_s flowie_protocol_repository_t;

typedef struct flowie_protocol_repository_option_s {
  const char *name;
  const char *value;
} flowie_protocol_repository_option_t;

typedef struct flowie_protocol_repository_limits_s {
  size_t max_sessions;
  size_t max_subscriptions_per_session;
  size_t max_inflight_per_session;
  size_t max_retained_messages;
  size_t max_client_id_size;
  size_t max_topic_size;
  size_t max_packet_size;
} flowie_protocol_repository_limits_t;

#define FLOWIE_PROTOCOL_REPOSITORY_LIMITS_INIT {0u, 0u, 0u, 0u, 0u, 0u, 0u}

typedef struct flowie_protocol_repository_config_s {
  size_t size;
  const char *driver;
  const flowie_protocol_repository_option_t *options;
  size_t option_count;
  const char *namespace_name;
  int create_schema;
  flowie_protocol_repository_limits_t limits;
} flowie_protocol_repository_config_t;

#define FLOWIE_PROTOCOL_REPOSITORY_CONFIG_INIT                                                \
  {sizeof(flowie_protocol_repository_config_t), NULL, NULL, 0u, NULL, 0,                      \
   FLOWIE_PROTOCOL_REPOSITORY_LIMITS_INIT}

typedef struct flowie_protocol_subscription_row_s {
  flowie_mqtt_span_t filter;
  uint8_t qos;
  uint8_t no_local;
  uint8_t retain_as_published;
  uint8_t retain_handling;
  uint32_t subscription_identifier;
} flowie_protocol_subscription_row_t;

typedef struct flowie_protocol_inflight_row_s {
  uint16_t packet_id;
  uint8_t qos;
} flowie_protocol_inflight_row_t;

typedef struct flowie_protocol_delivery_row_s {
  uint16_t packet_id;
  uint8_t qos;
  uint8_t state;
  uint64_t expiry_at_epoch_seconds;
  flowie_mqtt_span_t packet;
} flowie_protocol_delivery_row_t;

typedef struct flowie_protocol_will_row_s {
  int present;
  int pending;
  uint8_t qos;
  uint8_t retain;
  uint32_t delay_interval;
  flowie_mqtt_span_t topic;
  flowie_mqtt_span_t properties;
  flowie_mqtt_span_t payload;
} flowie_protocol_will_row_t;

/** Borrowed typed snapshot valid only for one synchronous repository call. */
typedef struct flowie_protocol_session_row_s {
  size_t size;
  flowie_mqtt_span_t client_id;
  uint64_t expected_revision;
  uint64_t revision;
  uint64_t session_id;
  uint64_t session_generation;
  flowie_mqtt_version_t mqtt_version;
  uint16_t keep_alive;
  uint32_t session_expiry_interval;
  uint16_t next_delivery_packet_id;
  uint64_t expiry_at_epoch_seconds;
  uint64_t will_at_epoch_seconds;
  int has_principal;
  flowie_security_principal_t principal;
  const flowie_protocol_subscription_row_t *subscriptions;
  size_t subscription_count;
  const flowie_protocol_inflight_row_t *inflight;
  size_t inflight_count;
  const flowie_protocol_delivery_row_t *deliveries;
  size_t delivery_count;
  flowie_protocol_will_row_t will;
} flowie_protocol_session_row_t;

#define FLOWIE_PROTOCOL_SESSION_ROW_INIT                                                     \
  {sizeof(flowie_protocol_session_row_t), {NULL, 0u}, 0u, 0u, 0u, 0u,                        \
   FLOWIE_MQTT_VERSION_UNSPECIFIED, 0u, 0u, 0u, 0u, 0u, 0, FLOWIE_SECURITY_PRINCIPAL_INIT,   \
   NULL, 0u, NULL, 0u, NULL, 0u, {0}}

typedef struct flowie_protocol_retained_row_s {
  size_t size;
  flowie_mqtt_span_t topic;
  uint64_t expected_revision;
  uint64_t revision;
  uint64_t publisher_session_id;
  uint64_t expiry_at_epoch_seconds;
  flowie_mqtt_version_t mqtt_version;
  uint8_t qos;
  flowie_mqtt_span_t properties;
  flowie_mqtt_span_t payload;
} flowie_protocol_retained_row_t;

#define FLOWIE_PROTOCOL_RETAINED_ROW_INIT                                                    \
  {sizeof(flowie_protocol_retained_row_t), {NULL, 0u}, 0u, 0u, 0u, 0u,                       \
   FLOWIE_MQTT_VERSION_UNSPECIFIED, 0u, {NULL, 0u}, {NULL, 0u}}

typedef int (*flowie_protocol_session_visit_fn)(void *ctx,
                                                 const flowie_protocol_session_row_t *row);
typedef int (*flowie_protocol_retained_visit_fn)(void *ctx,
                                                  const flowie_protocol_retained_row_t *row);

/**
 * Open a V2 ORM repository. Unknown or non-V2 schema data is rejected; it is never migrated.
 * The returned repository and all calls belong to one caller-serialized thread domain.
 */
FLOWIE_C_API int flowie_protocol_repository_open(
    const flowie_protocol_repository_config_t *config,
    flowie_protocol_repository_t **out);
FLOWIE_C_API void flowie_protocol_repository_close(flowie_protocol_repository_t *repository);

FLOWIE_C_API int flowie_protocol_repository_session_save(
    flowie_protocol_repository_t *repository, const flowie_protocol_session_row_t *row);
FLOWIE_C_API int flowie_protocol_repository_session_delete(
    flowie_protocol_repository_t *repository, flowie_mqtt_span_t client_id,
    uint64_t expected_revision);
FLOWIE_C_API int flowie_protocol_repository_session_visit(
    flowie_protocol_repository_t *repository, flowie_protocol_session_visit_fn visit,
    void *visit_ctx);

FLOWIE_C_API int flowie_protocol_repository_retained_save(
    flowie_protocol_repository_t *repository, const flowie_protocol_retained_row_t *row);
FLOWIE_C_API int flowie_protocol_repository_retained_delete(
    flowie_protocol_repository_t *repository, flowie_mqtt_span_t topic,
    uint64_t expected_revision);
FLOWIE_C_API int flowie_protocol_repository_retained_visit(
    flowie_protocol_repository_t *repository, flowie_protocol_retained_visit_fn visit,
    void *visit_ctx);

#ifdef __cplusplus
}
#endif

#endif
