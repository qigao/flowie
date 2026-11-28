#include "flowie_workflow_host_internal.h"
#include "tinytest.h"

#include <core/error.h>
#include <threads/threads.h>

#include <string.h>

enum { MAX_RESULTS = 8 };

typedef struct workflow_test_result_s {
  uint64_t id;
  flowie_workflow_phase_t phase;
  int status;
} workflow_test_result_t;

typedef struct workflow_test_s {
  sync_mutex_t mutex;
  sync_cond_t changed;
  flowie_workflow_decision_t decision;
  int drop_first;
  int stale_decision;
  int execute_status;
  int delivery_status;
  int startup_status;
  int stop_attempts;
  const void *owner_token;
  int block_execute;
  int block_delivery;
  size_t executes;
  size_t deliveries;
  size_t completed;
  workflow_test_result_t completions[MAX_RESULTS];
  unsigned char first_payload[2];
  char first_topic[5];
} workflow_test_t;

static int start(void *context) {
  workflow_test_t *test = context;
  test->owner_token = sync_thread_current_token();
  return test->startup_status;
}

static int stop(void *context) {
  workflow_test_t *test = context;
  if (test->owner_token != sync_thread_current_token()) return SALTS_EPERM;
  return ++test->stop_attempts == 1 ? SALTS_ETIMEDOUT : SALTS_OK;
}

static int decide(void *context, const flowie_workflow_event_t *event,
                  flowie_workflow_decision_t *decision) {
  workflow_test_t *test = context;
  sync_mutex_lock(&test->mutex);
  const size_t ordinal = ++test->executes;
  sync_cond_broadcast(&test->changed);
  while (test->block_execute) sync_cond_wait(&test->changed, &test->mutex);
  sync_mutex_unlock(&test->mutex);
  if (test->execute_status != SALTS_OK) return test->execute_status;
  *decision = test->decision;
  decision->generation = event->generation - test->stale_decision;
  decision->event_id = event->id;
  if (test->drop_first && ordinal == 1u) {
    decision->action = FLOWIE_WORKFLOW_DROP;
    decision->sink_size = 0;
  }
  return SALTS_OK;
}

static int deliver(void *context, const flowie_workflow_event_t *event) {
  workflow_test_t *test = context;
  sync_mutex_lock(&test->mutex);
  if (!test->deliveries && event->payload_size == sizeof(test->first_payload) &&
      event->topic_size == sizeof(test->first_topic)) {
    memcpy(test->first_payload, event->payload, sizeof(test->first_payload));
    memcpy(test->first_topic, event->topic, sizeof(test->first_topic));
  }
  ++test->deliveries;
  sync_cond_broadcast(&test->changed);
  while (test->block_delivery) sync_cond_wait(&test->changed, &test->mutex);
  sync_mutex_unlock(&test->mutex);
  return test->delivery_status;
}

static void complete(void *context, const flowie_workflow_event_t *event,
                     flowie_workflow_phase_t phase, int status) {
  workflow_test_t *test = context;
  if (test->completed < MAX_RESULTS) {
    test->completions[test->completed] = (workflow_test_result_t){event->id, phase, status};
  }
  ++test->completed;
}

static void unblock(workflow_test_t *test) {
  sync_mutex_lock(&test->mutex);
  test->block_execute = test->block_delivery = 0;
  sync_cond_broadcast(&test->changed);
  sync_mutex_unlock(&test->mutex);
}

static void wait_started(workflow_test_t *test, const size_t *counter) {
  sync_mutex_lock(&test->mutex);
  while (!*counter) sync_cond_wait(&test->changed, &test->mutex);
  sync_mutex_unlock(&test->mutex);
}

spec("Flowie resident workflow host") {
  static workflow_test_t test;
  static flowie_workflow_host_t *host;
  static flowie_workflow_host_config_t config;
  static flowie_workflow_event_t event;
  static flowie_workflow_sink_t sinks[2];

  before_each() {
    memset(&test, 0, sizeof(test));
    test.decision = (flowie_workflow_decision_t){.action = FLOWIE_WORKFLOW_FORWARD,
        .sink_size = sizeof("upstream") - 1u, .sink = "upstream"};
    host = NULL;
    sync_mutex_init(&test.mutex);
    sync_cond_init(&test.changed);
    sinks[0] = (flowie_workflow_sink_t){"upstream", deliver, &test};
    sinks[1] = (flowie_workflow_sink_t){"secondary", deliver, &test};
    config = (flowie_workflow_host_config_t){.generation = 7u,
        .event_capacity = 2u, .max_event_bytes = 12u, .max_pending_bytes = 14u,
        .sinks = sinks, .sink_count = 2u,
        .decide = decide, .complete = complete, .context = &test};
    event = (flowie_workflow_event_t){.generation = 7u, .id = 1u, .topic = "input",
        .topic_size = 5u, .payload = "\0\xA5", .payload_size = 2u};
  }

  after_each() {
    unblock(&test);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    sync_cond_destroy(&test.changed);
    sync_mutex_destroy(&test.mutex);
  }

  it("copies binary input and drains admitted items in FIFO order") {
    char topic[] = "input";
    unsigned char payload[] = {0, 0xa5};
    event.topic = topic;
    event.payload = payload;
    test.block_execute = 1;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    topic[0] = 'z';
    payload[1] = 0;
    event.id = 2u;
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    event.id = 3u;
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_ENOSPC);

    flowie_workflow_host_status_t status;
    flowie_workflow_host_snapshot(host, &status);
    check_equal(status.pending_events, 2u);
    check_equal(status.pending_bytes, 14u);
    check_equal(status.accepting, 1);
    unblock(&test);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);

    check_equal(test.executes, 2u);
    check_equal(test.deliveries, 2u);
    check_equal(test.completed, 2u);
    check_equal(test.completions[0].id, 1u);
    check_equal(test.completions[1].id, 2u);
    check_equal(test.completions[0].phase, FLOWIE_WORKFLOW_DELIVERY);
    check_equal(test.completions[0].status, SALTS_OK);
    check_equal(test.first_topic, "input", 5u);
    check_equal(test.first_payload[0], 0u);
    check_equal(test.first_payload[1], 0xa5u);
  }

  it("rejects a stale generation and exhaustion of the byte budget before the item limit") {
    config.max_pending_bytes = 12u;
    test.block_execute = 1;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    event.generation = 6u;
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_ECANCELED);
    event.generation = 7u;
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    event.id = 2u;
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_ENOSPC);
    unblock(&test);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.completed, 1u);
  }

  it("rejects ambiguous sink bindings at preparation") {
    sinks[1] = sinks[0];
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_EINVAL);
    check_null(host);
  }

  it("reports startup failure before publishing a host") {
    config.start = start;
    config.stop = stop;
    test.startup_status = SALTS_ENOSPC;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_ENOSPC);
    check_null(host);
    check_not_null(test.owner_token);
    check_equal(test.stop_attempts, 0);
  }

  it("keeps the worker alive when resource teardown needs another call") {
    config.start = start;
    config.stop = stop;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_ETIMEDOUT);
    check_not_null(host);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_ESHUTDOWN);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.completed, 1u);
    check_equal(test.stop_attempts, 2);
  }

  it("rejects an output outside the prepared sink names") {
    strcpy(test.decision.sink, "unconfigured");
    test.decision.sink_size = sizeof("unconfigured") - 1u;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.deliveries, 0u);
    check_equal(test.completed, 1u);
    check_equal(test.completions[0].phase, FLOWIE_WORKFLOW_SELECTION);
    check_equal(test.completions[0].status, SALTS_EPERM);
  }

  it("rejects embedded NUL in a sink name before selecting the backend") {
    static const char name[] = "upstream\0alias";
    memcpy(test.decision.sink, name, sizeof(name));
    test.decision.sink_size = sizeof(name) - 1u;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.deliveries, 0u);
    check_equal(test.completions[0].phase, FLOWIE_WORKFLOW_SELECTION);
    check_equal(test.completions[0].status, SALTS_EPROTO);
  }

  it("reports rule failure before making any downstream write") {
    test.execute_status = SALTS_EIO;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.deliveries, 0u);
    check_equal(test.completions[0].phase, FLOWIE_WORKFLOW_EXECUTION);
    check_equal(test.completions[0].status, SALTS_EIO);
  }

  it("filters the first message and forwards the second using the same rule context") {
    test.drop_first = 1;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    event.id = 2u;
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.executes, 2u);
    check_equal(test.deliveries, 1u);
    check_equal(test.completed, 2u);
    check_equal(test.completions[0].phase, FLOWIE_WORKFLOW_FILTERED);
    check_equal(test.completions[0].status, SALTS_OK);
    check_equal(test.completions[1].phase, FLOWIE_WORKFLOW_DELIVERY);
    check_equal(test.completions[1].status, SALTS_OK);
  }

  it("discards a decision from an earlier generation") {
    test.stale_decision = 1;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.deliveries, 0u);
    check_equal(test.completions[0].phase, FLOWIE_WORKFLOW_SELECTION);
    check_equal(test.completions[0].status, SALTS_EPROTO);
  }

  it("fences a running decision and cancels the rest of the queue on retirement") {
    test.block_execute = 1;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    wait_started(&test, &test.executes);
    event.id = 2u;
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    flowie_workflow_host_retire(host);
    event.id = 3u;
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_ESHUTDOWN);
    unblock(&test);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.executes, 1u);
    check_equal(test.deliveries, 0u);
    check_equal(test.completed, 2u);
    for (size_t i = 0; i < test.completed; ++i) {
      check_equal(test.completions[i].phase, FLOWIE_WORKFLOW_RETIREMENT);
      check_equal(test.completions[i].status, SALTS_ECANCELED);
    }
  }

  it("keeps the terminal result of a write admitted before retirement") {
    test.block_delivery = 1;
    test.delivery_status = SALTS_ETIMEDOUT;
    check_equal(flowie_workflow_host_create(&config, &host), SALTS_OK);
    check_equal(flowie_workflow_host_submit(host, &event), SALTS_OK);
    wait_started(&test, &test.deliveries);
    flowie_workflow_host_retire(host);
    unblock(&test);
    check_equal(flowie_workflow_host_destroy(&host), SALTS_OK);
    check_equal(test.deliveries, 1u);
    check_equal(test.completed, 1u);
    check_equal(test.completions[0].phase, FLOWIE_WORKFLOW_DELIVERY);
    check_equal(test.completions[0].status, SALTS_ETIMEDOUT);
  }
}
