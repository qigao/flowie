#ifndef FLOWIE_CONTROL_CONFIG_INTERNAL_H
#define FLOWIE_CONTROL_CONFIG_INTERNAL_H

#include "flowie_control_identity_internal.h"
#include "flowie_control_security_limits_internal.h"
#include "flowie_security.h"
#include "turbo_fs.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_CONTROL_CONFIG_VERSION 1u
#define FLOWIE_CONTROL_CONFIG_HOST_MAX 255u
#define FLOWIE_CONTROL_CONFIG_URL_MAX 2047u
#define FLOWIE_CONTROL_CONFIG_ROUTE_MAX 127u
#define FLOWIE_CONTROL_CONFIG_SECRET_REF_MAX 1024u
#define FLOWIE_CONTROL_CONFIG_ERROR_PATH_MAX 255u
#define FLOWIE_CONTROL_CONFIG_ERROR_MESSAGE_MAX 255u
#define FLOWIE_CONTROL_CONFIG_LISTENER_DEFAULT_COROUTINE_STACK_SIZE (256u * 1024u)
#define FLOWIE_CONTROL_CONFIG_LISTENER_MIN_COROUTINE_STACK_SIZE (256u * 1024u)
#define FLOWIE_CONTROL_CONFIG_LISTENER_MAX_COROUTINE_STACK_SIZE (2u * 1024u * 1024u)
#define FLOWIE_CONTROL_CONFIG_SESSION_DEFAULT_CAPACITY 1024u
#define FLOWIE_CONTROL_CONFIG_SESSION_MAX_CAPACITY 65536u
#define FLOWIE_CONTROL_CONFIG_SESSION_DEFAULT_MAX_PER_PRINCIPAL 5u
#define FLOWIE_CONTROL_CONFIG_SESSION_MAX_PER_PRINCIPAL 65536u
#define FLOWIE_CONTROL_CONFIG_SESSION_DEFAULT_TTL_SECONDS 3600u
#define FLOWIE_CONTROL_CONFIG_SESSION_MAX_TTL_SECONDS 86400u
#define FLOWIE_CONTROL_CONFIG_AUTH_CACHE_CAPACITY_MAX 4096u
#define FLOWIE_CONTROL_CONFIG_AUTH_CACHE_TTL_SECONDS_MAX 60u
#define FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_WORKERS 4u
#define FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_WORKERS 64u
#define FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_QUEUE_CAPACITY 128u
#define FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_QUEUE_CAPACITY 4096u
#define FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_DEADLINE_MS 10000u
#define FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_DEADLINE_MS 60000u
#define FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_DEFAULT_TIMEOUT_MS 3000u
#define FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_MAX_TIMEOUT_MS 30000u
#define FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_DEFAULT_RESPONSE_SIZE 16384u
#define FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_MIN_RESPONSE_SIZE 1024u
#define FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_MAX_RESPONSE_SIZE 65536u
#define FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_DEFAULT_MAX_IN_FLIGHT 64u
#define FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_MAX_IN_FLIGHT 1024u
#define FLOWIE_CONTROL_CONFIG_TURBODB_DRIVER_MAX 63u
#define FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX 16u
#define FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_KEY_MAX 63u
#define FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_VALUE_MAX 4095u
#define FLOWIE_CONTROL_CONFIG_BOOTSTRAP_PASSWORD_MIN 16u

typedef struct flowie_control_config_turbodb_option_s {
  char keyword[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_KEY_MAX + 1u];
  char value[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_VALUE_MAX + 1u];
} flowie_control_config_turbodb_option_t;

typedef struct flowie_control_config_turbodb_s {
  char driver[FLOWIE_CONTROL_CONFIG_TURBODB_DRIVER_MAX + 1u];
  flowie_control_config_turbodb_option_t options[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX];
  size_t option_count;
} flowie_control_config_turbodb_t;

typedef struct flowie_control_config_error_s {
  size_t size;
  int status;
  char path[FLOWIE_CONTROL_CONFIG_ERROR_PATH_MAX + 1u];
  char message[FLOWIE_CONTROL_CONFIG_ERROR_MESSAGE_MAX + 1u];
} flowie_control_config_error_t;

#define FLOWIE_CONTROL_CONFIG_ERROR_INIT {sizeof(flowie_control_config_error_t), 0, {0}, {0}}

typedef struct flowie_control_config_tls_s {
  char cert_file[TURBO_FS_MAX_PATH];
  char key_file[TURBO_FS_MAX_PATH];
  char client_ca_file[TURBO_FS_MAX_PATH];
  char key_password_ref[FLOWIE_CONTROL_CONFIG_SECRET_REF_MAX + 1u];
  int client_auth_required;
} flowie_control_config_tls_t;

typedef struct flowie_control_config_limits_s {
  size_t max_header_name_length;
  size_t max_header_value_length;
  size_t max_url_length;
  size_t max_cookie_name_length;
  size_t max_cookie_value_length;
  size_t max_json_depth;
  size_t max_log_message_length;
  size_t max_request_body_size;
  int max_headers_count;
} flowie_control_config_limits_t;

typedef struct flowie_control_config_listener_s {
  char host[FLOWIE_CONTROL_CONFIG_HOST_MAX + 1u];
  uint16_t port;
  size_t coroutine_stack_size;
  flowie_control_config_tls_t tls;
  flowie_control_config_limits_t limits;
} flowie_control_config_listener_t;

typedef struct flowie_control_config_management_s {
  char rpc_path[FLOWIE_CONTROL_CONFIG_ROUTE_MAX + 1u];
  size_t rpc_max_request_size;
  size_t session_capacity;
  size_t session_max_sessions_per_principal;
  uint64_t session_ttl_seconds;
  int login_executor_configured;
  uint32_t login_executor_workers;
  size_t login_executor_queue_capacity;
  uint32_t login_executor_deadline_ms;
} flowie_control_config_management_t;

typedef struct flowie_control_config_external_https_tls_s {
  char ca_file[TURBO_FS_MAX_PATH];
  char client_cert_file[TURBO_FS_MAX_PATH];
  char client_key_file[TURBO_FS_MAX_PATH];
  char client_key_password_ref[FLOWIE_CONTROL_CONFIG_SECRET_REF_MAX + 1u];
} flowie_control_config_external_https_tls_t;

typedef struct flowie_control_config_external_https_s {
  int enabled;
  char url[FLOWIE_CONTROL_CONFIG_URL_MAX + 1u];
  char service_token_ref[FLOWIE_CONTROL_CONFIG_SECRET_REF_MAX + 1u];
  char trusted_issuer[FLOWIE_SECURITY_ID_MAX + 1u];
  char subject_type[FLOWIE_SECURITY_TYPE_MAX + 1u];
  uint32_t timeout_ms;
  size_t max_response_size;
  uint32_t max_in_flight;
  flowie_control_config_external_https_tls_t tls;
} flowie_control_config_external_https_t;

typedef struct flowie_control_config_auth_local_executor_s {
  int configured;
  uint32_t workers;
  size_t queue_capacity;
  uint32_t deadline_ms;
} flowie_control_config_auth_local_executor_t;

typedef struct flowie_control_config_auth_s {
  int enabled;
  char listener_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char method[FLOWIE_SECURITY_TYPE_MAX + 1u];
  uint64_t principal_ttl_seconds;
  size_t credential_cache_capacity;
  uint64_t credential_cache_ttl_seconds;
  flowie_control_config_auth_local_executor_t local_executor;
  flowie_control_config_external_https_t external_https;
} flowie_control_config_auth_t;

typedef struct flowie_control_config_bootstrap_s {
  char domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char principal_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char principal_type[FLOWIE_SECURITY_TYPE_MAX + 1u];
} flowie_control_config_bootstrap_t;

typedef struct flowie_control_config_s {
  size_t size;
  uint32_t version;
  flowie_control_config_listener_t listener;
  flowie_control_config_turbodb_t turbodb;
  flowie_control_config_bootstrap_t bootstrap;
  flowie_control_config_management_t management;
  int dashboard_enabled;
  flowie_control_config_auth_t auth;
} flowie_control_config_t;

#define FLOWIE_CONTROL_CONFIG_INIT                                                                 \
  {                                                                                                \
    .size = sizeof(flowie_control_config_t), .version = FLOWIE_CONTROL_CONFIG_VERSION,             \
    .listener = {.port = 8443u,                                                                    \
                 .coroutine_stack_size =                                                           \
                     FLOWIE_CONTROL_CONFIG_LISTENER_DEFAULT_COROUTINE_STACK_SIZE,                  \
                 .limits = {128u, 4096u, 2048u, 128u, 4096u, 32u, 2048u, 65536u, 64}},             \
    .bootstrap = {FLOWIE_CONTROL_SYSTEM_DOMAIN, FLOWIE_CONTROL_SYSTEM_ADMIN_DEFAULT_USERNAME,      \
                  "human"},                                                                        \
    .management = {.rpc_max_request_size = 65536u,                                                 \
                   .session_capacity = FLOWIE_CONTROL_CONFIG_SESSION_DEFAULT_CAPACITY,             \
                   .session_max_sessions_per_principal =                                           \
                       FLOWIE_CONTROL_CONFIG_SESSION_DEFAULT_MAX_PER_PRINCIPAL,                    \
                   .session_ttl_seconds = FLOWIE_CONTROL_CONFIG_SESSION_DEFAULT_TTL_SECONDS,       \
                   .login_executor_workers =                                                       \
                       FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_WORKERS,                  \
                   .login_executor_queue_capacity =                                                \
                       FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_QUEUE_CAPACITY,           \
                   .login_executor_deadline_ms =                                                   \
                       FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_DEADLINE_MS},             \
    .dashboard_enabled = 1, .auth = {                                                              \
      .principal_ttl_seconds = 300u,                                                               \
      .credential_cache_capacity = 4096u,                                                          \
      .credential_cache_ttl_seconds = 60u,                                                         \
      .local_executor = {.workers = FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_WORKERS,     \
                         .queue_capacity =                                                         \
                             FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_QUEUE_CAPACITY,     \
                         .deadline_ms =                                                            \
                             FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_DEADLINE_MS},       \
      .external_https = {.timeout_ms = FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_DEFAULT_TIMEOUT_MS,    \
                         .max_response_size =                                                      \
                             FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_DEFAULT_RESPONSE_SIZE,           \
                         .max_in_flight =                                                          \
                             FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_DEFAULT_MAX_IN_FLIGHT}           \
    }                                                                                              \
  }

int flowie_control_config_parse_yaml(const char *yaml, size_t yaml_size,
                                     flowie_control_config_t *out,
                                     flowie_control_config_error_t *error);
int flowie_control_config_load(const char *path, flowie_control_config_t *out,
                               flowie_control_config_error_t *error);
int flowie_control_config_secret_ref_valid(const char *value);
int flowie_control_config_turbodb_secret_option(const char *keyword);

#ifdef __cplusplus
}
#endif

#endif
