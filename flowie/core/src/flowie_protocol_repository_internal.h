#ifndef FLOWIE_PROTOCOL_REPOSITORY_INTERNAL_H
#define FLOWIE_PROTOCOL_REPOSITORY_INTERNAL_H

#include "flowie_protocol_repository.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_replaced_will_row_s {
  uint64_t generation;
  flowie_mqtt_version_t version;
  flowie_mqtt_span_t packet;
} flowie_replaced_will_row_t;

typedef int (*flowie_protocol_session_restore_fn)(
    void *ctx, const flowie_protocol_session_row_t *row, const flowie_replaced_will_row_t *will);

int flowie_protocol_repository_replace_session(flowie_protocol_repository_t *repository,
                                               const flowie_protocol_session_row_t *row,
                                               const flowie_replaced_will_row_t *will);
int flowie_protocol_repository_complete_replaced_will(flowie_protocol_repository_t *repository,
                                                      flowie_mqtt_span_t client_id,
                                                      uint64_t generation);
int flowie_protocol_repository_restore_sessions(flowie_protocol_repository_t *repository,
                                                flowie_protocol_session_restore_fn visit,
                                                void *ctx);

#ifdef __cplusplus
}
#endif

#endif
