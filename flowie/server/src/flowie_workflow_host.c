#include "flowie_workflow_host_internal.h"

#include <core/error.h>
#include <threads/threads.h>

#include <stdlib.h>
#include <string.h>

typedef struct flowie_workflow_binding_s {
  char name[FLOWIE_WORKFLOW_SINK_NAME_CAPACITY];
  int (*deliver)(void *context, const flowie_workflow_event_t *event);
  void *context;
} flowie_workflow_binding_t;

struct flowie_workflow_host_s {
  flowie_workflow_host_config_t config;
  flowie_workflow_binding_t *sinks;
  flowie_workflow_event_t *events;
  unsigned char *bytes;
  sync_thread_t thread;
  sync_mutex_t mutex;
  sync_cond_t changed;
  const void *worker_token;
  size_t head;
  size_t tail;
  size_t pending_events;
  size_t pending_bytes;
  int started;
  int startup_status;
  int stop_requested;
  int stop_status;
  int closed;
  int retired;
};

static int flowie_workflow_name_valid(const char *name) {
  return name && name[0] && strlen(name) < FLOWIE_WORKFLOW_SINK_NAME_CAPACITY;
}

static int flowie_workflow_select(const flowie_workflow_host_t *host,
    const flowie_workflow_event_t *event, const flowie_workflow_decision_t *decision,
    const flowie_workflow_binding_t **out) {
  if (decision->generation != event->generation || decision->event_id != event->id)
    return SALTS_EPROTO;
  if (decision->action == FLOWIE_WORKFLOW_DROP)
    return decision->sink_size == 0 ? SALTS_OK : SALTS_EPROTO;
  if (decision->action != FLOWIE_WORKFLOW_FORWARD || decision->sink_size == 0 ||
      decision->sink_size >= sizeof(decision->sink) ||
      memchr(decision->sink, 0, decision->sink_size)) return SALTS_EPROTO;
  for (size_t i = 0; i < host->config.sink_count; ++i) {
    if (strlen(host->sinks[i].name) == decision->sink_size &&
        memcmp(host->sinks[i].name, decision->sink, decision->sink_size) == 0) {
      *out = &host->sinks[i];
      return SALTS_OK;
    }
  }
  return SALTS_EPERM;
}

static int flowie_workflow_run(flowie_workflow_host_t *host, const flowie_workflow_event_t *event,
                               flowie_workflow_phase_t *phase) {
  const flowie_workflow_binding_t *sink = NULL;
  flowie_workflow_decision_t decision = {0};
  *phase = FLOWIE_WORKFLOW_EXECUTION;
  int rc = host->config.decide(host->config.context, event, &decision);
  if (rc != SALTS_OK) return rc;
  *phase = FLOWIE_WORKFLOW_SELECTION;
  rc = flowie_workflow_select(host, event, &decision, &sink);
  if (rc != SALTS_OK) return rc;

  /* This lock orders write admission with retirement. The admitted operation runs outside it. */
  sync_mutex_lock(&host->mutex);
  const int retired = host->retired;
  sync_mutex_unlock(&host->mutex);
  if (retired) {
    *phase = FLOWIE_WORKFLOW_RETIREMENT;
    return SALTS_ECANCELED;
  }
  if (!sink) {
    *phase = FLOWIE_WORKFLOW_FILTERED;
    return SALTS_OK;
  }
  *phase = FLOWIE_WORKFLOW_DELIVERY;
  return sink->deliver(sink->context, event);
}

static void flowie_workflow_worker(void *context) {
  flowie_workflow_host_t *host = context;
  const int startup_status = host->config.start ? host->config.start(host->config.context) : SALTS_OK;
  sync_mutex_lock(&host->mutex);
  host->worker_token = sync_thread_current_token();
  host->startup_status = startup_status;
  host->started = 1;
  sync_cond_signal(&host->changed);
  while (startup_status == SALTS_OK) {
    while (!host->pending_events && !host->stop_requested)
      sync_cond_wait(&host->changed, &host->mutex);
    if (!host->pending_events) {
      sync_mutex_unlock(&host->mutex);
      const int rc = host->config.stop ? host->config.stop(host->config.context) : SALTS_OK;
      sync_mutex_lock(&host->mutex);
      host->stop_status = rc;
      host->stop_requested = 0;
      sync_cond_signal(&host->changed);
      if (rc == SALTS_OK) break;
      continue;
    }

    const flowie_workflow_event_t event = host->events[host->head];
    const int retired = host->retired;
    sync_mutex_unlock(&host->mutex);
    flowie_workflow_phase_t phase = FLOWIE_WORKFLOW_RETIREMENT;
    const int rc = retired ? SALTS_ECANCELED : flowie_workflow_run(host, &event, &phase);
    host->config.complete(host->config.context, &event, phase, rc);

    sync_mutex_lock(&host->mutex);
    host->pending_bytes -= event.topic_size + event.payload_size;
    --host->pending_events;
    host->head = (host->head + 1u) % host->config.event_capacity;
  }
  host->worker_token = NULL;
  sync_mutex_unlock(&host->mutex);
}

static void flowie_workflow_free(flowie_workflow_host_t *host) {
  free(host->bytes);
  free(host->events);
  free(host->sinks);
  free(host);
}

int flowie_workflow_host_create(const flowie_workflow_host_config_t *config,
                                flowie_workflow_host_t **out) {
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!config || !config->generation || !config->event_capacity || !config->max_event_bytes ||
      config->max_pending_bytes < config->max_event_bytes || !config->sinks || !config->sink_count ||
      !config->decide || !config->complete ||
      (!config->start != !config->stop))
    return SALTS_EINVAL;
  if (config->event_capacity > SIZE_MAX / config->max_event_bytes ||
      config->event_capacity > SIZE_MAX / sizeof(flowie_workflow_event_t) ||
      config->sink_count > SIZE_MAX / sizeof(flowie_workflow_binding_t))
    return SALTS_ERANGE;

  flowie_workflow_host_t *host = calloc(1, sizeof(*host));
  if (!host) return SALTS_ENOMEM;
  host->config = *config;
  host->config.sinks = NULL;
  host->sinks = calloc(config->sink_count, sizeof(*host->sinks));
  host->events = calloc(config->event_capacity, sizeof(*host->events));
  host->bytes = malloc(config->event_capacity * config->max_event_bytes);
  int rc = SALTS_ENOMEM;
  if (!host->sinks || !host->events || !host->bytes) goto fail;

  rc = SALTS_EINVAL;
  for (size_t i = 0; i < config->sink_count; ++i) {
    const flowie_workflow_sink_t *sink = &config->sinks[i];
    if (!flowie_workflow_name_valid(sink->name) || !sink->deliver) goto fail;
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(sink->name, host->sinks[j].name) == 0) goto fail;
    }
    strcpy(host->sinks[i].name, sink->name);
    host->sinks[i].deliver = sink->deliver;
    host->sinks[i].context = sink->context;
  }

  sync_mutex_init(&host->mutex);
  sync_cond_init(&host->changed);
  rc = sync_thread_create(&host->thread, flowie_workflow_worker, host);
  if (rc == SALTS_OK) {
    sync_mutex_lock(&host->mutex);
    while (!host->started) sync_cond_wait(&host->changed, &host->mutex);
    rc = host->startup_status;
    sync_mutex_unlock(&host->mutex);
    *out = host;
    if (rc != SALTS_OK) {
      const int cleanup = flowie_workflow_host_destroy(out);
      if (cleanup != SALTS_OK) return cleanup;
    }
    return rc;
  }
  sync_cond_destroy(&host->changed);
  sync_mutex_destroy(&host->mutex);
fail:
  flowie_workflow_free(host);
  return rc;
}

int flowie_workflow_host_submit(flowie_workflow_host_t *host, const flowie_workflow_event_t *event) {
  if (!host || !event || !event->id || !event->topic || !event->topic_size ||
      (!event->payload && event->payload_size))
    return SALTS_EINVAL;
  if (event->generation != host->config.generation) return SALTS_ECANCELED;
  if (event->topic_size > host->config.max_event_bytes ||
      event->payload_size > host->config.max_event_bytes - event->topic_size)
    return SALTS_ENOSPC;

  const size_t length = event->topic_size + event->payload_size;
  int rc = SALTS_OK;
  sync_mutex_lock(&host->mutex);
  if (host->closed) {
    rc = SALTS_ESHUTDOWN;
  } else if (host->pending_events == host->config.event_capacity ||
             length > host->config.max_pending_bytes - host->pending_bytes) {
    rc = SALTS_ENOSPC;
  } else {
    unsigned char *data = host->bytes + host->tail * host->config.max_event_bytes;
    memcpy(data, event->topic, event->topic_size);
    if (event->payload_size) memcpy(data + event->topic_size, event->payload, event->payload_size);
    host->events[host->tail] = *event;
    host->events[host->tail].topic = (const char *)data;
    host->events[host->tail].payload = event->payload_size ? data + event->topic_size : NULL;
    host->tail = (host->tail + 1u) % host->config.event_capacity;
    ++host->pending_events;
    host->pending_bytes += length;
    sync_cond_signal(&host->changed);
  }
  sync_mutex_unlock(&host->mutex);
  return rc;
}

void flowie_workflow_host_snapshot(flowie_workflow_host_t *host, flowie_workflow_host_status_t *out) {
  sync_mutex_lock(&host->mutex);
  *out = (flowie_workflow_host_status_t){host->pending_events, host->pending_bytes, !host->closed};
  sync_mutex_unlock(&host->mutex);
}

void flowie_workflow_host_retire(flowie_workflow_host_t *host) {
  sync_mutex_lock(&host->mutex);
  host->closed = 1;
  host->retired = 1;
  sync_cond_signal(&host->changed);
  sync_mutex_unlock(&host->mutex);
}

int flowie_workflow_host_destroy(flowie_workflow_host_t **owner) {
  if (!owner || !*owner) return SALTS_OK;
  flowie_workflow_host_t *host = *owner;
  sync_mutex_lock(&host->mutex);
  if (host->worker_token == sync_thread_current_token()) {
    sync_mutex_unlock(&host->mutex);
    return SALTS_EBUSY;
  }
  host->closed = 1;
  if (host->worker_token) {
    host->stop_requested = 1;
    sync_cond_signal(&host->changed);
    while (host->stop_requested) sync_cond_wait(&host->changed, &host->mutex);
    if (host->stop_status != SALTS_OK) {
      const int rc = host->stop_status;
      sync_mutex_unlock(&host->mutex);
      return rc;
    }
  }
  sync_mutex_unlock(&host->mutex);

  const int rc = sync_thread_join(&host->thread);
  if (rc != SALTS_OK) return rc;
  sync_cond_destroy(&host->changed);
  sync_mutex_destroy(&host->mutex);
  flowie_workflow_free(host);
  *owner = NULL;
  return SALTS_OK;
}
