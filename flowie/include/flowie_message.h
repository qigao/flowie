#ifndef FLOWIE_MESSAGE_H
#define FLOWIE_MESSAGE_H

#include "flowie_export.h"
#include "flowie_protocol_contract.h"
#include "flowie_mqtt_protocol.h"
#include "salts_buffer.h"
#include "salts_str.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One MQTT packet owned by Flowie while it crosses the broker/application boundary. */
typedef struct flowie_message_s {
  uint64_t id;
  uint64_t ts_ns;
  uint32_t type;
  uint32_t flags;
  mem_buffer_t *buffer;
  vstr payload;
  tstr owned_payload;
  void *transport_context;
  int status;
  uint32_t execution_attempt;
  flowie_protocol_route_t route;
  flowie_protocol_origin_t origin;
  flowie_protocol_settlement_envelope_t settlement;
  uint8_t has_route;
  uint8_t has_origin;
  uint8_t has_settlement;
} flowie_message_t;

typedef struct flowie_publish_result_s {
  size_t size;
  int status;
  flowie_protocol_settlement_point_t protocol_settlement;
} flowie_publish_result_t;

#define FLOWIE_MQTT_MESSAGE_BROKER_WILL UINT32_C(0x80000000)

#define FLOWIE_PUBLISH_RESULT_INIT                                                                 \
  {sizeof(flowie_publish_result_t), SALTS_OK, (flowie_protocol_settlement_point_t)0}

FLOWIE_C_API void flowie_message_init(flowie_message_t *message);
FLOWIE_C_API void flowie_message_cleanup(flowie_message_t *message);
FLOWIE_C_API int flowie_mqtt_message_flags_encode(flowie_mqtt_version_t version,
                                               uint8_t fixed_flags, uint32_t *flags_out);
FLOWIE_C_API int flowie_mqtt_message_flags_version(uint32_t flags,
                                                flowie_mqtt_version_t *version_out);
FLOWIE_C_API int flowie_message_set_protocol_route(flowie_message_t *message,
                                                const flowie_protocol_route_t *route);
FLOWIE_C_API const flowie_protocol_route_t *
flowie_message_protocol_route(const flowie_message_t *message);
FLOWIE_C_API int flowie_message_set_protocol_origin(flowie_message_t *message,
                                                 const flowie_protocol_origin_t *origin);
FLOWIE_C_API const flowie_protocol_origin_t *
flowie_message_protocol_origin(const flowie_message_t *message);
FLOWIE_C_API int flowie_message_set_protocol_settlement(
    flowie_message_t *message, const flowie_protocol_settlement_envelope_t *settlement);
FLOWIE_C_API const flowie_protocol_settlement_envelope_t *
flowie_message_protocol_settlement(const flowie_message_t *message);
FLOWIE_C_API int flowie_message_complete_protocol_settlement(
    flowie_message_t *message, flowie_protocol_settlement_point_t point);

#ifdef __cplusplus
}
#endif

#endif /* FLOWIE_MESSAGE_H */
