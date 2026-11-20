#ifndef FLOWIE_ENDPOINT_OWNER_LANE_INTERNAL_H
#define FLOWIE_ENDPOINT_OWNER_LANE_INTERNAL_H

#include "flow_execution.h"
#include "flowie_connection.h"

#include <stddef.h>

struct flowie_endpoint_s;

typedef struct flowie_endpoint_owner_lane_s {
  struct flowie_endpoint_s *endpoint;
  tf_execution_t execution_storage;
  flowie_server server_storage;
  tf_execution_t *execution;
  flowie_server *server;
  size_t index;
} flowie_endpoint_owner_lane_t;

/* owner must be live; lanes must contain lane_count live elements. Compare
 * identity only after bounding the index: unrelated pointers cannot be ordered. */
static inline int flowie_owner_lane_is_member(
    const flowie_endpoint_owner_lane_t *owner,
    const flowie_endpoint_owner_lane_t *lanes, size_t lane_count) {
  return owner != NULL && lanes != NULL && owner->index < lane_count &&
         owner == &lanes[owner->index];
}

#endif /* FLOWIE_ENDPOINT_OWNER_LANE_INTERNAL_H */
