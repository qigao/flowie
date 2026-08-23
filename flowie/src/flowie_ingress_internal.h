#ifndef FLOWIE_INGRESS_INTERNAL_H
#define FLOWIE_INGRESS_INTERNAL_H

#include "flowie_mqtt_protocol.h"
#include "flowie_message.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_ingress_s flowie_ingress_t;

typedef int (*flowie_ingress_prepare_fn)(void *ctx, flowie_ingress_t *ingress,
                                         const flowie_mqtt_packet_view_t *packet,
                                         int *publish_packet, int *stop_pump);

/** Same-lane dispatch into the endpoint's selected application composition. */
typedef int (*flowie_ingress_dispatch_fn)(void *ctx, flowie_message_t *message,
                                          flowie_publish_result_t *result);

/** Same-lane completion hook called once after a packet selected for dispatch. */
typedef int (*flowie_ingress_publish_complete_fn)(void *ctx, flowie_ingress_t *ingress,
                                                  const flowie_message_t *message,
                                                  const flowie_publish_result_t *result);

typedef struct flowie_ingress_config_s {
  size_t size;
  flowie_ingress_dispatch_fn dispatch;
  void *dispatch_ctx;
  flowie_mqtt_version_t version;
  size_t max_packet_size;
  /** Optional process-local route copied into every complete owned packet. */
  flowie_protocol_route_t route;
  /** Optional same-lane protocol-owner hook called before application delivery. */
  flowie_ingress_prepare_fn prepare;
  flowie_ingress_publish_complete_fn publish_complete;
  void *prepare_ctx;
} flowie_ingress_config_t;

#define FLOWIE_INGRESS_CONFIG_INIT                                                                 \
  {sizeof(flowie_ingress_config_t), NULL, NULL,                                                     \
   FLOWIE_MQTT_VERSION_UNSPECIFIED, FLOWIE_MQTT_MAX_WIRE_PACKET_SIZE,                              \
   FLOWIE_PROTOCOL_ROUTE_INIT, NULL, NULL, NULL}

FLOWIE_C_API flowie_ingress_t *flowie_ingress_create(const flowie_ingress_config_t *config);
FLOWIE_C_API void flowie_ingress_destroy(flowie_ingress_t *ingress);

/**
 * Connection-owner receive path. Socket, framing, and parsing must share one CoroNet lane.
 * A valid CONNECT must be first; it fixes the MQTT version for later packets.
 * Complete packets are dispatched through the required injected sink.
 * `published` counts only successful dispatches from this call.
 * A protocol, HWM, or dispatch error is terminal; destroy the ingress before accepting more bytes.
 */
FLOWIE_C_API int flowie_ingress_feed(flowie_ingress_t *ingress, const void *data, size_t size,
                                  size_t *published);

/** Resume parsing bytes already buffered after a prepare callback stopped the pump. */
FLOWIE_C_API int flowie_ingress_resume(flowie_ingress_t *ingress, size_t *published);

FLOWIE_C_API size_t flowie_ingress_buffered_bytes(const flowie_ingress_t *ingress);
FLOWIE_C_API flowie_mqtt_version_t flowie_ingress_version(const flowie_ingress_t *ingress);
/** MQTT 5 DISCONNECT reason for the terminal protocol error, or zero when not applicable. */
FLOWIE_C_API uint8_t flowie_ingress_disconnect_reason(const flowie_ingress_t *ingress);
FLOWIE_C_API int flowie_ingress_set_route(flowie_ingress_t *ingress,
                                       const flowie_protocol_route_t *route);
FLOWIE_C_API int flowie_ingress_set_protocol_settlement(
    flowie_ingress_t *ingress, const flowie_protocol_settlement_envelope_t *settlement);

/**
 * Replace the packet selected by the current prepare callback before application delivery.
 * The ingress copies `packet`; the override is consumed exactly once by the current pump step.
 */
FLOWIE_C_API int flowie_ingress_set_publish_packet(flowie_ingress_t *ingress, const void *packet,
                                                size_t packet_size);

#ifdef __cplusplus
}
#endif

#endif /* FLOWIE_INGRESS_INTERNAL_H */
