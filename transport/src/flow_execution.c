#include "flow_execution.h"

#include <salts/thread.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct tf_execution_call_s {
  tf_execution_call_fn fn;
  void *arg;
  salts_mutex_t mutex;
  salts_cond_t changed;
  atomic_int refs;
  int started;
  int canceled;
  int done;
  int status;
} tf_execution_call_t;

static size_t tf_execution_next_power_of_two(size_t value) {
  size_t result = 1u;
  while (result < value && result <= SIZE_MAX / 2u)
    result *= 2u;
  return result < value ? 0u : result;
}

static void tf_execution_call_release(tf_execution_call_t *call) {
  if (call == NULL || atomic_fetch_sub_explicit(&call->refs, 1, memory_order_acq_rel) != 1)
    return;
  salts_cond_destroy(&call->changed);
  salts_mutex_destroy(&call->mutex);
  free(call);
}

static void tf_execution_call_run(coro_t *coroutine, void *arg) {
  tf_execution_call_t *call = (tf_execution_call_t *)arg;
  int status;
  (void)coroutine;
  salts_mutex_lock(&call->mutex);
  if (call->canceled) {
    salts_mutex_unlock(&call->mutex);
    return;
  }
  call->started = 1;
  salts_cond_signal(&call->changed);
  salts_mutex_unlock(&call->mutex);
  status = call->fn(call->arg);
  salts_mutex_lock(&call->mutex);
  call->status = status;
  call->done = 1;
  salts_cond_signal(&call->changed);
  salts_mutex_unlock(&call->mutex);
}

static void tf_execution_call_cancel(void *arg, int status) {
  tf_execution_call_t *call = (tf_execution_call_t *)arg;
  salts_mutex_lock(&call->mutex);
  call->started = 1;
  call->status = status;
  call->done = 1;
  salts_cond_signal(&call->changed);
  salts_mutex_unlock(&call->mutex);
}

static void tf_execution_call_finalize(void *arg) {
  tf_execution_call_release((tf_execution_call_t *)arg);
}

int tf_execution_init(tf_execution_t *execution, const flowie_execution_binding_t *binding,
                      size_t coroutine_capacity, size_t stack_size) {
  int status;
  if (execution == NULL || coroutine_capacity == 0u || coroutine_capacity > SIZE_MAX / 2u)
    return SALTS_EINVAL;
  memset(execution, 0, sizeof(*execution));
  status = flowie_execution_binding_validate(binding);
  if (status != SALTS_OK) return status;
  execution->kind = binding->kind;
  if (binding->kind == FLOWIE_EXECUTION_PRIVATE) {
    salts_coro_executor_config_t config = SALTS_CORO_EXECUTOR_CONFIG_DEFAULT;
    config.worker_count = 1u;
    config.queue_capacity_per_worker = tf_execution_next_power_of_two(coroutine_capacity * 2u);
    config.coroutine_pool.initial_capacity = coroutine_capacity < 16u ? coroutine_capacity : 16u;
    config.coroutine_pool.max_capacity = coroutine_capacity;
    config.coroutine_pool.stack_size = stack_size;
    if (config.queue_capacity_per_worker == 0u) return SALTS_ERANGE;
    execution->executor = salts_coro_executor_create(&config);
    if (execution->executor == NULL) return SALTS_ENOMEM;
    execution->owns_executor = 1;
    execution->shard = 0u;
  } else {
    execution->executor = binding->executor;
    execution->shard = binding->shard;
    execution->owns_executor = binding->kind == FLOWIE_EXECUTION_OWNED_EXECUTOR;
  }
  return SALTS_OK;
}

int tf_execution_start(tf_execution_t *execution) {
  return execution == NULL || execution->executor == NULL ? SALTS_EINVAL : SALTS_OK;
}

int tf_execution_post(tf_execution_t *execution, coro_fn fn, void *arg) {
  salts_coro_executor_task_t task;
  if (execution == NULL || execution->executor == NULL || fn == NULL) return SALTS_EINVAL;
  task = (salts_coro_executor_task_t){fn, NULL, NULL, arg};
  return salts_coro_executor_submit_to(execution->executor, execution->shard, &task);
}

static int tf_execution_call_common(tf_execution_t *execution, tf_execution_call_fn fn, void *arg,
                                    uint64_t timeout_ns) {
  tf_execution_call_t *call;
  salts_coro_executor_task_t task;
  int status;
  if (execution == NULL || execution->executor == NULL || fn == NULL || timeout_ns == 0u)
    return SALTS_EINVAL;
  if (salts_coro_executor_current() == execution->executor &&
      salts_coro_executor_current_shard(execution->executor) == execution->shard)
    return fn(arg);
  call = (tf_execution_call_t *)calloc(1u, sizeof(*call));
  if (call == NULL) return SALTS_ENOMEM;
  call->fn = fn;
  call->arg = arg;
  call->status = SALTS_EALREADY;
  atomic_init(&call->refs, 2);
  salts_mutex_init(&call->mutex);
  salts_cond_init(&call->changed);
  if (call->mutex == NULL || call->changed == NULL) {
    atomic_store(&call->refs, 1);
    tf_execution_call_release(call);
    return SALTS_ENOMEM;
  }
  task = (salts_coro_executor_task_t){tf_execution_call_run, tf_execution_call_cancel,
                                      tf_execution_call_finalize, call};
  status = salts_coro_executor_submit_to(execution->executor, execution->shard, &task);
  if (status != SALTS_OK) {
    tf_execution_call_release(call);
    tf_execution_call_release(call);
    return status;
  }
  salts_mutex_lock(&call->mutex);
  while (!call->started && !call->done) {
    if (salts_cond_timedwait(&call->changed, &call->mutex, timeout_ns) != SALTS_OK) break;
  }
  if (!call->started && !call->done) {
    call->canceled = 1;
    status = SALTS_ETIMEDOUT;
  } else {
    while (!call->done)
      salts_cond_wait(&call->changed, &call->mutex);
    status = call->status;
  }
  salts_mutex_unlock(&call->mutex);
  tf_execution_call_release(call);
  return status;
}

int tf_execution_call(tf_execution_t *execution, tf_execution_call_fn fn, void *arg,
                      uint64_t timeout_ns) {
  return tf_execution_call_common(execution, fn, arg, timeout_ns);
}

int tf_execution_call_coro(tf_execution_t *execution, tf_execution_call_fn fn, void *arg,
                           uint64_t timeout_ns) {
  return tf_execution_call_common(execution, fn, arg, timeout_ns);
}

void tf_execution_stop(tf_execution_t *execution) {
  (void)execution;
}

void tf_execution_destroy(tf_execution_t *execution) {
  if (execution == NULL) return;
  if (execution->owns_executor && execution->executor != NULL) {
    (void)salts_coro_executor_shutdown(execution->executor);
    (void)salts_coro_executor_wait(execution->executor);
    (void)salts_coro_executor_destroy(execution->executor);
  }
  memset(execution, 0, sizeof(*execution));
}
