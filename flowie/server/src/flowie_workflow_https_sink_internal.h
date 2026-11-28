#ifndef FLOWIE_WORKFLOW_HTTPS_SINK_INTERNAL_H
#define FLOWIE_WORKFLOW_HTTPS_SINK_INTERNAL_H

#include "flowie_workflow_host_internal.h"
#include <http_client/http.h>

typedef struct flowie_workflow_https_sink_s {
  chttp_client *client;
  chttp_options request;
  unsigned int *http_status;
} flowie_workflow_https_sink_t;

/* The prepared binding borrows one client and immutable HTTPS options through the worker's
 * join. The worker creates, drives and destroys the client. The event supplies the body.
 * One POST is attempted;
 * a 2xx response succeeds, a non-2xx response returns EPROTO, and transport errors propagate. */
int flowie_workflow_https_deliver(void *context, const flowie_workflow_event_t *event);

#endif
