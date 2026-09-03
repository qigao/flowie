#ifndef FLOW_EXECUTION_H
#define FLOW_EXECUTION_H

#include "flowie_execution.h"

typedef struct tf_execution_s {
  salts_coro_executor_t *executor;
  size_t shard;
  flowie_execution_kind_t kind;
  int owns_executor;
} tf_execution_t;

typedef int (*tf_execution_call_fn)(void *arg);

int tf_execution_init(tf_execution_t *execution, const flowie_execution_binding_t *binding,
                      size_t coroutine_capacity, size_t stack_size);
int tf_execution_start(tf_execution_t *execution);
int tf_execution_post(tf_execution_t *execution, coro_fn fn, void *arg);
int tf_execution_call(tf_execution_t *execution, tf_execution_call_fn fn, void *arg,
                      uint64_t timeout_ns);
int tf_execution_call_coro(tf_execution_t *execution, tf_execution_call_fn fn, void *arg,
                           uint64_t timeout_ns);
void tf_execution_stop(tf_execution_t *execution);
void tf_execution_destroy(tf_execution_t *execution);

#endif /* FLOW_EXECUTION_H */
