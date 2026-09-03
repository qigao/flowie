#ifndef FLOWIE_EXECUTION_H
#define FLOWIE_EXECUTION_H

#include <salts/error_codes.h>
#include <salts_coro_executor.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum flowie_execution_kind_e {
  /** Flowie creates and owns one dedicated Salts executor shard. */
  FLOWIE_EXECUTION_PRIVATE = 1,
  /** The host owns the executor for the complete Flowie endpoint lifetime. */
  FLOWIE_EXECUTION_BORROWED_EXECUTOR,
  /** Flowie takes ownership of the supplied executor and destroys it. */
  FLOWIE_EXECUTION_OWNED_EXECUTOR
} flowie_execution_kind_t;

/** Immutable Salts executor placement, copied during endpoint registration. */
typedef struct flowie_execution_binding_s {
  /** Must be at least sizeof(flowie_execution_binding_t). */
  size_t size;
  flowie_execution_kind_t kind;
  salts_coro_executor_t *executor;
  /** Stable zero-based owner shard for all endpoint state transitions. */
  size_t shard;
  uint32_t flags;
} flowie_execution_binding_t;

#define FLOWIE_EXECUTION_BINDING_INIT                                                        \
  {sizeof(flowie_execution_binding_t), FLOWIE_EXECUTION_PRIVATE, NULL, 0u, 0u}

static inline int flowie_execution_binding_validate(
    const flowie_execution_binding_t *binding) {
  salts_coro_executor_stats_t stats = {0};
  if (binding == NULL || binding->size < sizeof(*binding) || binding->flags != 0u)
    return SALTS_EINVAL;
  if (binding->kind == FLOWIE_EXECUTION_PRIVATE)
    return binding->executor == NULL && binding->shard == 0u ? SALTS_OK : SALTS_EINVAL;
  if ((binding->kind != FLOWIE_EXECUTION_BORROWED_EXECUTOR &&
       binding->kind != FLOWIE_EXECUTION_OWNED_EXECUTOR) ||
      binding->executor == NULL)
    return SALTS_EINVAL;
  salts_coro_executor_get_stats(binding->executor, &stats);
  return binding->shard < stats.worker_count ? SALTS_OK : SALTS_ERANGE;
}

#ifdef __cplusplus
}
#endif

#endif /* FLOWIE_EXECUTION_H */
