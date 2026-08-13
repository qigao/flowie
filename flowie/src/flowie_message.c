#include "flowie_message.h"

#include "turbo_error.h"

#include <string.h>

static int flowie_route_validate(const flowie_protocol_route_t *route) {
  return route && route->size >= sizeof(*route) &&
                 route->contract_version == FLOWIE_PROTOCOL_CONTRACT_VERSION &&
                 route->protocol == FLOWIE_PROTOCOL_MQTT && route->owner_instance_id != 0u &&
                 route->session_id != 0u && route->session_generation != 0u
             ? TURBO_OK
             : TURBO_EINVAL;
}

int flowie_mqtt_message_flags_encode(flowie_mqtt_version_t version, uint8_t fixed_flags,
                                     uint32_t *flags_out) {
  if (!flags_out || !flowie_mqtt_version_is_supported(version) || fixed_flags > 0x0fu)
    return TURBO_EINVAL;
  *flags_out = ((uint32_t)version << 8u) | fixed_flags;
  return TURBO_OK;
}

int flowie_mqtt_message_flags_version(uint32_t flags, flowie_mqtt_version_t *version_out) {
  flowie_mqtt_version_t version;
  if (!version_out) return TURBO_EINVAL;
  version = (flowie_mqtt_version_t)((flags >> 8u) & 0xffu);
  if (!flowie_mqtt_version_is_supported(version)) return TURBO_EPROTO;
  *version_out = version;
  return TURBO_OK;
}

void flowie_message_init(flowie_message_t *message) {
  if (!message) return;
  memset(message, 0, sizeof(*message));
  message->route = (flowie_protocol_route_t)FLOWIE_PROTOCOL_ROUTE_INIT;
  message->origin = (flowie_protocol_origin_t)FLOWIE_PROTOCOL_ORIGIN_INIT;
  message->settlement =
      (flowie_protocol_settlement_envelope_t)FLOWIE_PROTOCOL_SETTLEMENT_ENVELOPE_INIT;
  message->status = TURBO_OK;
}

void flowie_message_cleanup(flowie_message_t *message) {
  if (!message) return;
  if (message->buffer) mem_buffer_release(message->buffer);
  tstr_freep(&message->owned_payload);
  flowie_message_init(message);
}

int flowie_message_set_protocol_route(flowie_message_t *message,
                                      const flowie_protocol_route_t *route) {
  if (!message || flowie_route_validate(route) != TURBO_OK) return TURBO_EINVAL;
  message->route = *route;
  message->route.size = sizeof(message->route);
  message->has_route = 1u;
  return TURBO_OK;
}

const flowie_protocol_route_t *flowie_message_protocol_route(const flowie_message_t *message) {
  return message && message->has_route ? &message->route : NULL;
}

int flowie_message_set_protocol_origin(flowie_message_t *message,
                                       const flowie_protocol_origin_t *origin) {
  if (!message || flowie_protocol_origin_validate(origin) != TURBO_OK) return TURBO_EINVAL;
  message->origin = *origin;
  message->origin.size = sizeof(message->origin);
  message->has_origin = 1u;
  return TURBO_OK;
}

const flowie_protocol_origin_t *flowie_message_protocol_origin(const flowie_message_t *message) {
  return message && message->has_origin ? &message->origin : NULL;
}

int flowie_message_set_protocol_settlement(
    flowie_message_t *message, const flowie_protocol_settlement_envelope_t *settlement) {
  if (!message || !message->has_route || !settlement || settlement->size < sizeof(*settlement) ||
      settlement->contract_version != FLOWIE_PROTOCOL_CONTRACT_VERSION ||
      flowie_protocol_message_validate(&settlement->message) != TURBO_OK ||
      settlement->requested_point < FLOWIE_PROTOCOL_SETTLE_RECEIVED ||
      settlement->requested_point > FLOWIE_PROTOCOL_SETTLE_DURABLE ||
      settlement->settled_point != 0) {
    return TURBO_EINVAL;
  }
  if (message->has_settlement) return TURBO_EALREADY;
  if (settlement->message.protocol != message->route.protocol ||
      settlement->message.session_generation != message->route.session_generation) {
    return TURBO_EPROTO;
  }
  message->settlement = *settlement;
  message->settlement.size = sizeof(message->settlement);
  message->has_settlement = 1u;
  return TURBO_OK;
}

const flowie_protocol_settlement_envelope_t *
flowie_message_protocol_settlement(const flowie_message_t *message) {
  return message && message->has_settlement ? &message->settlement : NULL;
}

int flowie_message_complete_protocol_settlement(flowie_message_t *message,
                                                flowie_protocol_settlement_point_t point) {
  if (!message || !message->has_settlement || point != message->settlement.requested_point)
    return TURBO_EINVAL;
  if (message->settlement.settled_point != 0) return TURBO_EALREADY;
  message->settlement.settled_point = point;
  return TURBO_OK;
}
