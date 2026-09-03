#ifndef FLOWIE_SERVER_CONFIG_INTERNAL_H
#define FLOWIE_SERVER_CONFIG_INTERNAL_H

#include "flowie_security.h"

#include <stddef.h>
#include <stdint.h>

typedef struct flowie_server_config_s flowie_server_config_t;
typedef struct flowie_endpoint_config_s flowie_endpoint_config_t;

typedef struct flowie_server_http_provider_config_s {
  char url[2049];
  char method[FLOWIE_SECURITY_TYPE_MAX + 1u];
  char service_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char service_domain[FLOWIE_SECURITY_ID_MAX + 1u];
  char service_token_ref[FLOWIE_SECURITY_SECRET_REF_MAX + 1u];
  char ca_file[1025];
  uint32_t timeout_ms;
  size_t max_body_size;
} flowie_server_http_provider_config_t;

typedef struct flowie_server_config_error_s {
  size_t size;
  int status;
  char path[512];
  char message[256];
} flowie_server_config_error_t;

#define FLOWIE_SERVER_CONFIG_ERROR_INIT {sizeof(flowie_server_config_error_t), 0, {0}, {0}}

int flowie_server_config_load(const char *path, const char *profile, int require_security,
                              flowie_server_config_t **out,
                              flowie_server_config_error_t *error);
void flowie_server_config_destroy(flowie_server_config_t *config);

const flowie_endpoint_config_t *flowie_server_config_endpoint(
    const flowie_server_config_t *config);
const char *flowie_server_config_endpoint_name(const flowie_server_config_t *config);
const char *flowie_server_config_realm_name(const flowie_server_config_t *config);
const char *flowie_server_config_realm_resource_uid(const flowie_server_config_t *config);
const char *flowie_server_config_realm_owner_name(const flowie_server_config_t *config);
const char *flowie_server_config_acl_provider_name(const flowie_server_config_t *config);
const char *flowie_server_config_auth_method(const flowie_server_config_t *config);
const flowie_server_http_provider_config_t *flowie_server_config_auth(
    const flowie_server_config_t *config);
const flowie_server_http_provider_config_t *flowie_server_config_acl(
    const flowie_server_config_t *config);

#endif
