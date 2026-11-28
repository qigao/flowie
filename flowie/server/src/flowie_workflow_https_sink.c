#include "flowie_workflow_https_sink_internal.h"

#include <core/error.h>

#include <string.h>

int flowie_workflow_https_deliver(void *context, const flowie_workflow_event_t *event) {
  const flowie_workflow_https_sink_t *sink = context;
  if (!sink || !event || !sink->client || !sink->request.connection_uri ||
      strncmp(sink->request.connection_uri, "tls://", sizeof("tls://") - 1u) != 0 ||
      !sink->request.tls || !sink->request.timeout_ms || sink->request.body_source ||
      sink->request.body_sink)
    return SALTS_EINVAL;

  chttp_options request = sink->request;
  request.body = event->payload;
  request.body_size = event->payload_size;
  chttp_response response = {0};
  chttp_error error = {0};
  int rc = chttp_post(sink->client, &request, &response, &error);
  if (sink->http_status) *sink->http_status = rc == SALTS_OK ? response.status_code : 0u;
  if (rc == SALTS_OK && (response.status_code < 200u || response.status_code >= 300u))
    rc = SALTS_EPROTO;
  chttp_response_destroy(&response);
  return rc;
}
