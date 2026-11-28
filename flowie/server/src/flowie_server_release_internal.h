#ifndef FLOWIE_SERVER_RELEASE_INTERNAL_H
#define FLOWIE_SERVER_RELEASE_INTERNAL_H

#include "flowie_server_config_internal.h"
#include "flowie.h"

typedef struct flowie_server_release_s flowie_server_release_t;

/* Exclusive lifecycle on the server control thread. Borrow the parsed profile
 * through destroy. Prepare restores the durable generation with ingress closed.
 * Even on error, a non-NULL *out owns the partially prepared resources and
 * must be stopped and destroyed by the enclosing runtime. */
int flowie_server_release_prepare(const flowie_server_release_config_t *config,
    const orm_config_t *database, int check_only, flowie_server_release_t **out,
    const char **operation);
int flowie_server_release_start(flowie_server_release_t *release);
int flowie_server_release_reconcile(flowie_server_release_t *release,
    flowie_protocol_repository_t *repository);
int flowie_server_release_stop(flowie_server_release_t *release, int *settled);
int flowie_server_release_port(const flowie_server_release_t *release, uint16_t *port);
/* After the Broker stops, retain the owner if a downstream client is still draining. */
int flowie_server_release_destroy(flowie_server_release_t **owner);
int flowie_server_release_publish(flowie_server_release_t *release, const flowie_message_t *message,
    int *handled);

/* Borrow until the broker endpoint has stopped and been destroyed. */
const flowie_endpoint_security_binding_t *flowie_server_release_binding(
    const flowie_server_release_t *release);

#endif
