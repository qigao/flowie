#ifndef FLOWIE_SERVER_CONFIG_INTERNAL_H
#define FLOWIE_SERVER_CONFIG_INTERNAL_H

#include "flowie_security.h"

#include <stddef.h>
#include <stdint.h>

typedef struct flowie_server_config_s flowie_server_config_t;
typedef struct flowie_endpoint_config_s flowie_endpoint_config_t;

/* Strict option parsing; failure leaves the corresponding fields unchanged. */
int flowie_server_network_policy_parse(const char *text, flowie_endpoint_config_t *config);
int flowie_server_network_cpus_parse(const char *text, flowie_endpoint_config_t *config);

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

enum { FLOWIE_SERVER_RELEASE_MAX_CLIENTS = 64, FLOWIE_SERVER_RELEASE_PIN_LENGTH = 64,
       FLOWIE_SERVER_RELEASE_PATH_CAPACITY = 1025, FLOWIE_SERVER_RELEASE_SM2_ID_CAPACITY = 8192,
       FLOWIE_SERVER_RULESFORGE_MAX_SINKS = 8, FLOWIE_SERVER_RULESFORGE_SINK_NAME = 64,
       FLOWIE_SERVER_RULESFORGE_MAX_TIMEOUT_MS = 2000 };

typedef struct flowie_server_release_client_s {
  char principal[FLOWIE_SECURITY_ID_MAX + 1u];
  char sha256[FLOWIE_SERVER_RELEASE_PIN_LENGTH + 1u];
} flowie_server_release_client_t;

typedef struct flowie_server_praktor_config_s {
  char url[2049];
  char ca_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char cert_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char key_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  uint32_t timeout_ms;
} flowie_server_praktor_config_t;

typedef struct flowie_server_rulesforge_sink_s {
  char name[FLOWIE_SERVER_RULESFORGE_SINK_NAME + 1];
  char url[2049];
  char ca_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char cert_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char key_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
} flowie_server_rulesforge_sink_t;

typedef struct flowie_server_rulesforge_config_s {
  char executable[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char provider[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char staging_root[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char journal_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  uint32_t timeout_ms;
  size_t sink_count;
  flowie_server_rulesforge_sink_t sinks[FLOWIE_SERVER_RULESFORGE_MAX_SINKS];
} flowie_server_rulesforge_config_t;

typedef struct flowie_server_release_config_s {
  char domain[FLOWIE_SECURITY_ID_MAX + 1u];
  char environment[FLOWIE_SECURITY_ID_MAX + 1u];
  char validation_policy_revision[FLOWIE_SECURITY_ID_MAX + 1u];
  char management_host[256];
  uint16_t management_port;
  char management_cert_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char management_key_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char management_client_ca_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char mqtt_client_ca_file[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char operator_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char operator_sha256[FLOWIE_SERVER_RELEASE_PIN_LENGTH + 1u];
  flowie_server_http_provider_config_t management_authority;
  char signer_reference[FLOWIE_SECURITY_ID_MAX + 1u];
  char signer_anchor_der[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char signer_leaf_der[FLOWIE_SERVER_RELEASE_PATH_CAPACITY];
  char signer_identity[FLOWIE_SERVER_RELEASE_SM2_ID_CAPACITY];
  char issuer_identity[FLOWIE_SERVER_RELEASE_SM2_ID_CAPACITY];
  size_t client_count;
  flowie_server_release_client_t clients[FLOWIE_SERVER_RELEASE_MAX_CLIENTS];
  flowie_server_http_provider_config_t auth;
  int64_t package_count;
  int64_t package_bytes;
  flowie_server_praktor_config_t praktor;
  flowie_server_rulesforge_config_t rulesforge;
} flowie_server_release_config_t;

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
/* Borrowed immutable deployment configuration. NULL selects the existing broker profile. */
const flowie_server_release_config_t *flowie_server_config_release(
    const flowie_server_config_t *config);

#endif
