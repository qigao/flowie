#ifndef FLOWIE_RULESFORGE_PIPELINE_INTERNAL_H
#define FLOWIE_RULESFORGE_PIPELINE_INTERNAL_H

#include "flowie_control_release_mqtt_internal.h"
#include "flowie_server_config_internal.h"

typedef struct flowie_rulesforge_pipeline_s flowie_rulesforge_pipeline_t;

typedef struct flowie_rulesforge_pipeline_provider_s {
  const flowie_server_release_config_t *config;
  flowie_control_release_store_t *store;
  /* At most the retired predecessor and the last active generation can need cleanup. */
  flowie_rulesforge_pipeline_t *retired[2];
} flowie_rulesforge_pipeline_provider_t;

/* Deployment configuration borrows through release destruction. */
int flowie_rulesforge_pipeline_prepare(void *context, const flowie_release_archive_t *archive,
    void **out);
int flowie_rulesforge_pipeline_close(void *context, void *handle);
int flowie_rulesforge_pipeline_reap(flowie_rulesforge_pipeline_provider_t *provider);
int flowie_rulesforge_pipeline_publish(void *handle, const flowie_control_release_active_t *active,
    const flowie_security_principal_t *principal, const flowie_mqtt_publish_view_t *publish,
    const uint8_t *exchange, int *handled);
void flowie_rulesforge_pipeline_activate(void *handle, const flowie_control_release_active_t *active,
    int recovered);

#endif
