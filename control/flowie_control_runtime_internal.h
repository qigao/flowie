#ifndef FLOWIE_CONTROL_RUNTIME_INTERNAL_H
#define FLOWIE_CONTROL_RUNTIME_INTERNAL_H

#include "flowie_control_config_internal.h"
#include "flowie_control_management_service_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_control_runtime_s flowie_control_runtime_t;

/** Validate TLS identity, secret references, and store selection without opening a listener/DB. */
int flowie_control_runtime_validate(const flowie_control_config_t *config);

/**
 * Assemble Repository, Identity and Application through the Component Configurator.
 * Routes are bound without publishing the listener. Failure leaves *out NULL and
 * releases partially created resources; committed bootstrap data remains persistent.
 */
int flowie_control_runtime_create(const flowie_control_config_t *config,
                                  flowie_control_runtime_t **out);

/**
 * Start and stop the owned CHttp::Server HTTPS listener without installing process signal handlers.
 *
 * These calls are caller-serialized. start() binds the configured endpoint before it returns;
 * stop() drains deferred replies, closes accepted connections, and joins the CHTTP owner.
 */
int flowie_control_runtime_start(flowie_control_runtime_t *runtime);
int flowie_control_runtime_stop(flowie_control_runtime_t *runtime);

/** Run the configured HTTPS/mTLS listener until SIGINT or SIGTERM. */
int flowie_control_runtime_run(flowie_control_runtime_t *runtime);

/**
 * Drain request handling before stopping the component graph in reverse dependency order.
 * On stop failure the runtime remains owned by the caller and must not be freed.
 */
int flowie_control_runtime_destroy(flowie_control_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
