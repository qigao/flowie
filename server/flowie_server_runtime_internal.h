#ifndef FLOWIE_SERVER_RUNTIME_INTERNAL_H
#define FLOWIE_SERVER_RUNTIME_INTERNAL_H

#include "flowie.h"
#include "flowie_server_config_internal.h"

typedef struct flowie_server_runtime_s flowie_server_runtime_t;

/* Exclusive product lifecycle. Config/database and callback contexts are borrowed
 * through destroy. check_only assembles the same graph without starting a listener.
 * Failure returns the original Salts error and leaves *out NULL after rollback;
 * operation, when supplied, receives a static diagnostic phase name. */
int flowie_server_runtime_create(const flowie_server_config_t *config,
                                 const flowie_endpoint_config_t *endpoint_config,
                                 const orm_config_t *database,
                                 const flowie_endpoint_core_options_t *options,
                                 int check_only, flowie_server_runtime_t **out,
                                 const char **operation);

/* Borrows the active graph; the view expires before destroy returns. */
flowie_endpoint_core_t *flowie_server_runtime_endpoint(const flowie_server_runtime_t *runtime);

/* Stops the endpoint before releasing repository/security dependencies. NULL is
 * idempotent. A Component lifecycle error preserves *runtime for diagnosis/retry. */
int flowie_server_runtime_destroy(flowie_server_runtime_t **runtime);

#endif
