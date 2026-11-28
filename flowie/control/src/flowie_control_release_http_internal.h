#ifndef FLOWIE_CONTROL_RELEASE_HTTP_INTERNAL_H
#define FLOWIE_CONTROL_RELEASE_HTTP_INTERNAL_H

#include "flowie_control_release_mqtt_internal.h"
#include "flowie_control_release_access_internal.h"

#include <cnet/cnet.h>

typedef struct flowie_control_release_http_s flowie_control_release_http_t;

/* Submits verified YAML or the complete verified USTAR when it has companion
 * files. SALTS_OK records acceptance by the execution service. Arguments
 * borrow only for the call. */
typedef int (*flowie_control_release_praktor_submit_fn)(void *context, const char *domain,
    const char *digest, const char *key, const char *type, const void *body, size_t size);

typedef struct flowie_control_release_http_config_s {
  const char *host;
  uint16_t port;
  const cnet_tls_server_config *tls;
  flowie_control_release_mqtt_t *mqtt;
  const char *operator_id;
  /* Lowercase SHA-256 hex of the TLS operator certificate, without a prefix. */
  const char *operator_certificate_sha256;
  const char *signer_reference;
  const char *validation_policy_revision;
  flowie_control_release_praktor_submit_fn submit_praktor;
  void *praktor_context;
  flowie_release_authorize_fn authorize;
  void *authorize_context;
} flowie_control_release_http_config_t;

/* Owns the HTTPS listener. Requires verified client TLS and the configured operator pin.
 * The runtime and immutable strings borrow through close; TLS config borrows through open.
 * Stop the listener before destroying the runtime or its store. */
int flowie_control_release_http_open(const flowie_control_release_http_config_t *config,
    flowie_control_release_http_t **out);
/* Prepare all credentials and bindings with ingress closed. Start on the same control thread. */
int flowie_control_release_http_prepare(const flowie_control_release_http_config_t *config,
    flowie_control_release_http_t **out);
int flowie_control_release_http_start(flowie_control_release_http_t *server);
int flowie_control_release_http_port(const flowie_control_release_http_t *server, uint16_t *port);
/* Clears the owner when cleanup settles. A live owner remains available after a failed close. */
int flowie_control_release_http_close(flowie_control_release_http_t **owner);

#endif
