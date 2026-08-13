#ifndef FLOWIE_EXECUTION_H
#define FLOWIE_EXECUTION_H

#include "CoroNet/turbo_coro_thread_pool.h"
#include "turbo_error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum flowie_execution_kind_e {
  /** Adapter creates, drives, stops, and destroys a dedicated context. */
  FLOWIE_EXECUTION_PRIVATE = 1,
  /** Host drives and owns context for the complete adapter lifetime. */
  FLOWIE_EXECUTION_BORROWED_CONTEXT,
  /** Adapter exclusively drives, stops, and destroys the supplied context. */
  FLOWIE_EXECUTION_OWNED_CONTEXT,
  /** Host pool drives and owns the stable context selected by lane. */
  FLOWIE_EXECUTION_POOL_LANE
} flowie_execution_kind_t;

/** Immutable adapter execution placement, copied during registration. */
typedef struct flowie_execution_binding_s {
  /** Must be at least sizeof(flowie_execution_binding_t). */
  size_t size;
  flowie_execution_kind_t kind;
  coro_context_t *context;
  coro_thread_pool_t *pool;
  uint32_t lane;
  uint32_t flags;
} flowie_execution_binding_t;

/**
 * Validate field combinations and resolve a pool lane.
 *
 * @return TURBO_OK, TURBO_EINVAL for an invalid combination, or TURBO_ERANGE
 * for a lane not present in pool. This function does not take ownership.
 */
static inline int flowie_execution_binding_validate(
    const flowie_execution_binding_t *binding) {
  if (!binding || binding->size < sizeof(*binding) || binding->flags != 0u) return TURBO_EINVAL;
  switch (binding->kind) {
  case FLOWIE_EXECUTION_PRIVATE:
    return !binding->context && !binding->pool && binding->lane == 0u ? TURBO_OK : TURBO_EINVAL;
  case FLOWIE_EXECUTION_BORROWED_CONTEXT:
  case FLOWIE_EXECUTION_OWNED_CONTEXT:
    return binding->context && !binding->pool && binding->lane == 0u ? TURBO_OK : TURBO_EINVAL;
  case FLOWIE_EXECUTION_POOL_LANE:
    if (binding->context || !binding->pool) return TURBO_EINVAL;
    return coro_thread_pool_get_context(binding->pool, (int)binding->lane) ? TURBO_OK
                                                                           : TURBO_ERANGE;
  default:
    return TURBO_EINVAL;
  }
}

#ifdef __cplusplus
}
#endif

#endif /* FLOWIE_EXECUTION_H */

