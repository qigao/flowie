#ifndef FLOWIE_MQTT_SECURITY_H
#define FLOWIE_MQTT_SECURITY_H

#include "flowie_export.h"
#include "flowie_mqtt_protocol.h"
#include "flowie_security.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum flowie_mqtt_security_resource_kind_e {
  FLOWIE_MQTT_SECURITY_TOPIC = 1,
  FLOWIE_MQTT_SECURITY_TOPIC_FILTER
} flowie_mqtt_security_resource_kind_t;

typedef struct flowie_mqtt_security_context_s {
  size_t size;
  flowie_mqtt_security_resource_kind_t kind;
  flowie_mqtt_span_t username;
  flowie_mqtt_span_t client_id;
} flowie_mqtt_security_context_t;

#define FLOWIE_MQTT_SECURITY_CONTEXT_INIT                                                          \
  {sizeof(flowie_mqtt_security_context_t), FLOWIE_MQTT_SECURITY_TOPIC, {NULL, 0u}, {NULL, 0u}}

FLOWIE_C_API int flowie_mqtt_security_matcher_init(flowie_security_matcher_t *out);

#ifdef __cplusplus
}
#endif

#endif
