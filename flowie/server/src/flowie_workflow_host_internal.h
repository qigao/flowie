#ifndef FLOWIE_WORKFLOW_HOST_INTERNAL_H
#define FLOWIE_WORKFLOW_HOST_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct flowie_workflow_host_s flowie_workflow_host_t;

typedef struct flowie_workflow_event_s {
  uint64_t generation;
  uint64_t id;
  const char *topic;
  size_t topic_size;
  const void *payload;
  size_t payload_size;
} flowie_workflow_event_t;

typedef enum flowie_workflow_phase_e {
  FLOWIE_WORKFLOW_EXECUTION,
  FLOWIE_WORKFLOW_SELECTION,
  FLOWIE_WORKFLOW_DELIVERY,
  FLOWIE_WORKFLOW_RETIREMENT,
  FLOWIE_WORKFLOW_FILTERED
} flowie_workflow_phase_t;

enum { FLOWIE_WORKFLOW_SINK_NAME_CAPACITY = 65 };

typedef enum flowie_workflow_action_e {
  FLOWIE_WORKFLOW_DROP = 1,
  FLOWIE_WORKFLOW_FORWARD
} flowie_workflow_action_t;

typedef struct flowie_workflow_decision_s {
  uint64_t generation;
  uint64_t event_id;
  flowie_workflow_action_t action;
  size_t sink_size;
  char sink[FLOWIE_WORKFLOW_SINK_NAME_CAPACITY];
} flowie_workflow_decision_t;

typedef struct flowie_workflow_sink_s {
  const char *name;
  int (*deliver)(void *context, const flowie_workflow_event_t *event);
  void *context;
} flowie_workflow_sink_t;

typedef struct flowie_workflow_host_config_s {
  uint64_t generation;
  size_t event_capacity;
  size_t max_event_bytes;
  size_t max_pending_bytes;
  const flowie_workflow_sink_t *sinks;
  size_t sink_count;
  int (*start)(void *context);
  int (*stop)(void *context);
  int (*decide)(void *context, const flowie_workflow_event_t *event,
                flowie_workflow_decision_t *decision);
  void (*complete)(void *context, const flowie_workflow_event_t *event,
                   flowie_workflow_phase_t phase, int status);
  void *context;
} flowie_workflow_host_config_t;

typedef struct flowie_workflow_host_status_s {
  size_t pending_events;
  size_t pending_bytes;
  int accepting;
} flowie_workflow_host_status_t;

/* One already prepared, immutable Domain generation. Sink bindings are copied.
 * Callback contexts stay valid through destroy. Callbacks run serially on one worker, enforce
 * their own finite deadlines, and borrow the event only for the call. Provide start and stop
 * together to initialize and release thread-affine resources. Failed start rolls back its own
 * resources before returning. On success decide returns one event-correlated decision.
 * A drop names no sink. A forward names a configured sink. deliver returns the final outcome
 * of an admitted write. complete reports exactly one outcome for each accepted event. */
int flowie_workflow_host_create(const flowie_workflow_host_config_t *config,
                                flowie_workflow_host_t **out);

/* Multi-producer FIFO admission. Copies topic and payload on success. Limits include the event
 * on the worker. A full queue returns ENOSPC, a different generation ECANCELED and a closed
 * generation ESHUTDOWN. Failed admission has no completion callback. */
int flowie_workflow_host_submit(flowie_workflow_host_t *host, const flowie_workflow_event_t *event);
void flowie_workflow_host_snapshot(flowie_workflow_host_t *host, flowie_workflow_host_status_t *out);

/* Close admission and fence writes. A write admitted before the fence can finish. Any other
 * accepted event either reports its earlier failure or gets a retirement completion. */
void flowie_workflow_host_retire(flowie_workflow_host_t *host);

/* Exclusive destruction after submitters and observers quiesce. Close admission, drain accepted
 * events, stop worker-owned resources and join the worker. Calling from a worker callback returns
 * EBUSY. A failed stop keeps the worker and its closed host alive for another destroy call.
 * A failed join also preserves the closed host. */
int flowie_workflow_host_destroy(flowie_workflow_host_t **host);

#endif
