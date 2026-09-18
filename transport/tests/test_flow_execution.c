#include "flow_execution.h"

#include "tinytest.h"

#include <salts/error_codes.h>

#include <stdatomic.h>

typedef struct flow_execution_probe_s {
  atomic_int posts;
  int call_status;
} flow_execution_probe_t;

static void flow_execution_post(coro_t *coroutine, void *arg) {
  flow_execution_probe_t *probe = (flow_execution_probe_t *)arg;
  (void)coroutine;
  atomic_fetch_add_explicit(&probe->posts, 1, memory_order_release);
}

static int flow_execution_call(void *arg) {
  flow_execution_probe_t *probe = (flow_execution_probe_t *)arg;
  return probe->call_status;
}

spec("Flowie Salts execution binding") {
  it("serializes private endpoint work on one executor shard") {
    const flowie_execution_binding_t binding = FLOWIE_EXECUTION_BINDING_INIT;
    flow_execution_probe_t probe = {0};
    tf_execution_t execution = {0};

    probe.call_status = SALTS_EINTR;
    check_equal(tf_execution_init(&execution, &binding, 8u, 0u), SALTS_OK);
    check_equal(tf_execution_start(&execution), SALTS_OK);
    check_equal(tf_execution_post(&execution, flow_execution_post, &probe), SALTS_OK);
    check_equal(tf_execution_call(&execution, flow_execution_call, &probe,
                                  UINT64_C(1000000000)),
                SALTS_EINTR);
    check_equal(atomic_load_explicit(&probe.posts, memory_order_acquire), 1);
    tf_execution_stop(&execution);
    tf_execution_destroy(&execution);
  }

  it("validates a borrowed Salts executor shard") {
    salts_coro_executor_config_t config = SALTS_CORO_EXECUTOR_CONFIG_DEFAULT;
    salts_coro_executor_t *executor;
    flowie_execution_binding_t binding = FLOWIE_EXECUTION_BINDING_INIT;
    tf_execution_t execution = {0};

    config.worker_count = 2u;
    config.queue_capacity_per_worker = 4u;
    config.coroutine_pool.max_capacity = 4u;
    executor = salts_coro_executor_create(&config);
    check_not_null(executor);
    binding.kind = FLOWIE_EXECUTION_BORROWED_EXECUTOR;
    binding.executor = executor;
    binding.shard = 1u;
    check_equal(flowie_execution_binding_validate(&binding), SALTS_OK);
    check_equal(tf_execution_init(&execution, &binding, 4u, 0u), SALTS_OK);
    tf_execution_destroy(&execution);
    check_equal(salts_coro_executor_destroy(executor), SALTS_OK);
  }
}
