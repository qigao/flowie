#ifndef FLOWIE_RULESFORGE_WORKER_INTERNAL_H
#define FLOWIE_RULESFORGE_WORKER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

enum {
  FLOWIE_RULESFORGE_NAME_MAX = 128,
  FLOWIE_RULESFORGE_BODY_MAX = 16 * 1024,
  FLOWIE_RULESFORGE_OUTPUT_MAX = 32,
  FLOWIE_RULESFORGE_FRAME_MAX = 1024 * 1024
};

typedef struct flowie_rulesforge_worker_s flowie_rulesforge_worker_t;

typedef struct flowie_rulesforge_output_s {
  const char *sink;
  size_t sink_size;
  const void *body;
  size_t body_size;
} flowie_rulesforge_output_t;

typedef struct flowie_rulesforge_batch_s {
  uint64_t id;
  uint64_t expired;
  int drain_required;
  size_t output_count;
  flowie_rulesforge_output_t outputs[FLOWIE_RULESFORGE_OUTPUT_MAX];
} flowie_rulesforge_batch_t;

/* One exclusive owner. program and bundle are deployment-selected paths. Open
 * waits for readiness within timeout_ms. On failure all process and pipe resources
 * are released. The caller stages the trusted bundle before calling open. */
int flowie_rulesforge_worker_open(const char *program, const char *bundle,
    uint32_t timeout_ms, flowie_rulesforge_worker_t **out);

/* One batch at a time. Output views borrow worker storage until the next
 * exchange or close. No write or read is retried after an ambiguous failure. */
int flowie_rulesforge_worker_push(flowie_rulesforge_worker_t *worker, int64_t time_ms,
    const char *event_id, const char *entry_point, const char *fact_type,
    const void *json, size_t json_size, flowie_rulesforge_batch_t *batch);
int flowie_rulesforge_worker_watermark(flowie_rulesforge_worker_t *worker, int64_t time_ms,
    flowie_rulesforge_batch_t *batch);
int flowie_rulesforge_worker_drain(flowie_rulesforge_worker_t *worker,
    flowie_rulesforge_batch_t *batch);
int flowie_rulesforge_worker_ack(flowie_rulesforge_worker_t *worker, uint64_t batch_id);

/* A clean close requires all batches acknowledged and rule work drained.
 * Faulted or unsettled workers are terminated and reaped. Always clears owner. */
int flowie_rulesforge_worker_close(flowie_rulesforge_worker_t **owner);

#endif
