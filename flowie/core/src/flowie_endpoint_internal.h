#ifndef FLOWIE_ENDPOINT_INTERNAL_H
#define FLOWIE_ENDPOINT_INTERNAL_H

#include "flow_connection.h"
#include "flowie.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_endpoint_core_snapshot_s {
  size_t size;
  flowie_connection_snapshot_t connection;
  size_t queued_replies;
  size_t sessions;
} flowie_endpoint_core_snapshot_t;

#define FLOWIE_ENDPOINT_CORE_SNAPSHOT_INIT                                                         \
  {sizeof(flowie_endpoint_core_snapshot_t), {0}, 0u, 0u}

/** Borrowed, point-in-time diagnostic view used by in-tree lifecycle tests. */
FLOWIE_C_API int flowie_endpoint_core_snapshot(flowie_endpoint_core_t *core,
                                               flowie_endpoint_core_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif
