#include "flowie_control_database_config_internal.h"
#include "flowie_control_http_request_internal.h"
#include "flowie_control_runtime_internal.h"

#include "flowie_control_acl_iris_endpoint_internal.h"
#include "flowie_control_auth_iris_adapter_internal.h"
#include "flowie_control_auth_iris_endpoint_internal.h"
#include "flowie_control_bootstrap_internal.h"
#include "flowie_control_dashboard_internal.h"
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
  #include "flowie_control_external_https_authenticator_internal.h"
#endif
#if defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
  #include "flowie_control_jwt_jwks_authenticator_internal.h"
#endif
#include "flowie_control_management_rpc_internal.h"
#include "flowie_control_management_session_internal.h"
#include "flowie_control_service_credential_internal.h"
#include "monocypher.h"
#include "salts_error.h"
#include "salts_thread.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#define FLOWIE_CONTROL_AUTH_ENDPOINT_BODY_MAX 8192u
#define FLOWIE_CONTROL_RUNTIME_SESSION_CONTEXT "/.flowie/internal/session-runtime"

struct flowie_control_runtime_s {
  flowie_control_config_t config;
  flowie_control_store_t *store;
  const flowie_control_repository_t *repository;
  flowie_control_management_service_t *management_service;
  flowie_control_auth_service_t *management_auth_service;
  flowie_control_management_session_store_t *management_sessions;
  rpc_context_t *rpc_context;
  flowie_control_http_app_t *app;
  flowie_control_management_rpc_server_t *management_rpc;
  flowie_control_dashboard_t *dashboard;
  flowie_control_auth_service_t *auth_service;
  flowie_control_service_credential_resolver_t *service_credentials;
  flowie_control_auth_iris_adapter_t *auth_adapter;
  flowie_control_auth_iris_endpoint_t *auth_endpoint;
  flowie_control_acl_iris_endpoint_t *acl_endpoint;
  int listener_started;
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
  flowie_control_external_https_authenticator_t *external_https_authenticator;
#endif
#if defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
  flowie_control_jwt_jwks_authenticator_t *jwt_jwks_authenticator;
#endif
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH) || defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
  flowie_control_external_subject_mapper_t *external_subject_mapper;
#endif
};

static volatile sig_atomic_t flowie_control_runtime_stop_requested;

static void flowie_control_runtime_signal(int signal_number) {
  (void)signal_number;
  flowie_control_runtime_stop_requested = 1;
}

#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
static int flowie_control_runtime_validate_external_https(const flowie_control_config_t *config);
#endif
#if defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
static int flowie_control_runtime_validate_jwt_jwks(const flowie_control_config_t *config);
#endif

static int flowie_control_runtime_external_auth_enabled(const flowie_control_config_t *config) {
  return config && (config->auth.external_https.enabled || config->auth.jwt_jwks.enabled);
}

static uint64_t flowie_control_runtime_clock(void *ctx) {
  (void)ctx;
  return salts_realtime_ms() / 1000u;
}

static int flowie_control_runtime_external_https_stats(
    void *ctx, flowie_control_external_https_authenticator_stats_t *stats_out) {
  flowie_control_runtime_t *runtime = (flowie_control_runtime_t *)ctx;
  if (!runtime || !stats_out || stats_out->size < sizeof(*stats_out)) return SALTS_EINVAL;
  *stats_out = (flowie_control_external_https_authenticator_stats_t)
      FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_STATS_INIT;
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
  if (!runtime->external_https_authenticator) return SALTS_ENOENT;
  return flowie_control_external_https_authenticator_get_stats(
      runtime->external_https_authenticator, stats_out);
#else
  return SALTS_ENOENT;
#endif
}

static int flowie_control_runtime_routes_valid(const flowie_control_config_t *config) {
  if (!config || !config->management.rpc_path[0]) return 0;
  if (strcmp(config->management.rpc_path, FLOWIE_CONTROL_RUNTIME_SESSION_CONTEXT) == 0) return 0;
  if (flowie_control_runtime_external_auth_enabled(config) && !config->auth.enabled) return 0;
  if (config->auth.enabled &&
      strcmp(config->management.rpc_path, FLOWIE_CONTROL_AUTH_HTTP_PATH) == 0)
    return 0;
  if (config->auth.enabled &&
      strcmp(config->management.rpc_path, FLOWIE_CONTROL_ACL_HTTP_PATH) == 0)
    return 0;
  if (config->dashboard_enabled &&
      flowie_control_dashboard_path_reserved(config->management.rpc_path))
    return 0;
  return 1;
}

static int flowie_control_runtime_env_secret(const char *reference, const char **value_out) {
  static const char prefix[] = "env://";
  const char *value;
  const char *name;
  if (value_out) *value_out = NULL;
  if (!reference || !value_out) return SALTS_EINVAL;
  if (!reference[0]) return SALTS_OK;
  if (!flowie_control_config_secret_ref_valid(reference)) return SALTS_EINVAL;
  name = reference + sizeof(prefix) - 1u;
  value = getenv(name);
  if (!value || !value[0]) return SALTS_ENOENT;
  *value_out = value;
  return SALTS_OK;
}

static int flowie_control_runtime_tls_config(const flowie_control_config_t *config,
                                             flowie_control_http_tls_config_t *tls_out) {
  const char *password = NULL;
  int rc;
  if (!config || config->size < sizeof(*config) ||
      config->version != FLOWIE_CONTROL_CONFIG_VERSION || !tls_out)
    return SALTS_EINVAL;
  rc = flowie_control_runtime_env_secret(config->listener.tls.key_password_ref, &password);
  if (rc != SALTS_OK) return rc;
  *tls_out = (flowie_control_http_tls_config_t){
      sizeof(*tls_out),
      config->listener.tls.cert_file,
      config->listener.tls.key_file,
      password,
      config->listener.tls.client_auth_required ? config->listener.tls.client_ca_file : NULL,
      config->listener.tls.client_auth_required};
  return SALTS_OK;
}

static int flowie_control_runtime_turbodb_config(
    const flowie_control_config_t *config, orm_config_t *database,
    orm_option_t options[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX]) {
  return flowie_control_database_config_resolve(config, database, options);
}

static int flowie_control_runtime_validate_store(const flowie_control_config_t *config) {
  orm_config_t database;
  orm_option_t options[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX];
  return flowie_control_runtime_turbodb_config(config, &database, options);
}

int flowie_control_runtime_validate(const flowie_control_config_t *config) {
  flowie_control_http_tls_config_t tls = FLOWIE_CONTROL_HTTP_TLS_CONFIG_INIT;
  int external_auth;
  int rc;
  if (!flowie_control_runtime_routes_valid(config)) return SALTS_EINVAL;
  if (config->auth.external_https.enabled && config->auth.jwt_jwks.enabled) return SALTS_EINVAL;
  external_auth = flowie_control_runtime_external_auth_enabled(config);
  if (config->listener.coroutine_stack_size <
          FLOWIE_CONTROL_CONFIG_LISTENER_MIN_COROUTINE_STACK_SIZE ||
      config->listener.coroutine_stack_size >
          FLOWIE_CONTROL_CONFIG_LISTENER_MAX_COROUTINE_STACK_SIZE)
    return SALTS_EINVAL;
  if (config->dashboard_enabled && config->listener.tls.client_auth_required) return SALTS_EINVAL;
  if ((external_auth && config->management.login_executor_configured) ||
      (!external_auth && (config->management.login_executor_workers == 0u ||
                          config->management.login_executor_workers >
                              FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_WORKERS ||
                          config->management.login_executor_queue_capacity == 0u ||
                          config->management.login_executor_queue_capacity >
                              FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_QUEUE_CAPACITY ||
                          config->management.login_executor_deadline_ms == 0u ||
                          config->management.login_executor_deadline_ms >
                              FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_DEADLINE_MS)))
    return SALTS_EINVAL;
  if (strcmp(config->bootstrap.domain_id, FLOWIE_CONTROL_SYSTEM_DOMAIN) != 0 ||
      strcmp(config->bootstrap.principal_id, FLOWIE_CONTROL_SYSTEM_ADMIN_DEFAULT_USERNAME) != 0 ||
      strcmp(config->bootstrap.principal_type, "human") != 0 ||
      sizeof(FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD) - 1u <
          FLOWIE_CONTROL_CONFIG_BOOTSTRAP_PASSWORD_MIN ||
      sizeof(FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD) - 1u >
          FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX)
    return SALTS_EINVAL;
  if (config->auth.enabled &&
      ((external_auth && config->auth.local_executor.configured) ||
       (!external_auth && (config->auth.local_executor.workers == 0u ||
                           config->auth.local_executor.workers >
                               FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_WORKERS ||
                           config->auth.local_executor.queue_capacity == 0u ||
                           config->auth.local_executor.queue_capacity >
                               FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_QUEUE_CAPACITY ||
                           config->auth.local_executor.deadline_ms == 0u ||
                           config->auth.local_executor.deadline_ms >
                               FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_DEADLINE_MS))))
    return SALTS_EINVAL;
  rc = flowie_control_runtime_validate_store(config);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_runtime_tls_config(config, &tls);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_http_tls_validate(&tls);
  if (rc == SALTS_OK && config->auth.external_https.enabled) {
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
    rc = flowie_control_runtime_validate_external_https(config);
#else
    rc = SALTS_ENOTSUP;
#endif
  }
  if (rc == SALTS_OK && config->auth.jwt_jwks.enabled) {
#if defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
    rc = flowie_control_runtime_validate_jwt_jwks(config);
#else
    rc = SALTS_ENOTSUP;
#endif
  }

  return rc;
}

static int flowie_control_runtime_request_token(
    const Req *request, char output[FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u]) {
  static const char prefix[] = "Bearer ";
  const char *authorization;
  char cookie[FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u] = {0};
  const char *bearer = NULL;
  size_t bearer_size = 0u;
  int has_cookie;
  int rc = SALTS_EPERM;
  if (output) output[0] = '\0';
  if (!request || !output) return SALTS_EINVAL;
  has_cookie = flowie_control_http_cookie_exact(request, FLOWIE_CONTROL_MANAGEMENT_SESSION_COOKIE,
                                                cookie, sizeof(cookie)) == SALTS_OK;
  if (flowie_control_http_header_exact(request, "Authorization", &authorization) != SALTS_OK)
    authorization = NULL;
  if (authorization && strncmp(authorization, prefix, sizeof(prefix) - 1u) == 0) {
    bearer = authorization + sizeof(prefix) - 1u;
    bearer_size = strnlen(bearer, FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u);
  }
  if (has_cookie && bearer && strcmp(cookie, bearer) != 0) goto done;
  if (bearer && bearer_size == FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE)
    memcpy(output, bearer, bearer_size + 1u);
  else if (has_cookie &&
           strnlen(cookie, sizeof(cookie)) == FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE)
    memcpy(output, cookie, FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u);
  else goto done;
  rc = SALTS_OK;

done:
  crypto_wipe(cookie, sizeof(cookie));
  return rc;
}

static int flowie_control_runtime_session_middleware(Req *request, Res *response, Chain *chain) {
  flowie_control_runtime_t *runtime;
  flowie_control_management_session_identity_t *identity;
  char token[FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u] = {0};
  int rc;
  if (!request || !response || !chain || !request->security || !request->app) return 1;
  runtime = (flowie_control_runtime_t *)flowie_control_http_app_lookup_context(
      request->app, FLOWIE_CONTROL_RUNTIME_SESSION_CONTEXT);
  if (!runtime) return next(chain, request, response);
  rc = flowie_control_runtime_request_token(request, token);
  if (rc != SALTS_OK) return next(chain, request, response);
  identity = (flowie_control_management_session_identity_t *)calloc(1u, sizeof(*identity));
  if (!identity) {
    crypto_wipe(token, sizeof(token));
    send_json(response, INTERNAL_SERVER_ERROR, "{\"error\":\"Session unavailable\"}");
    return 1;
  }
  identity->size = sizeof(*identity);
  rc = flowie_control_management_session_resolve(runtime->management_sessions, token, identity);
  crypto_wipe(token, sizeof(token));
  if (rc != SALTS_OK) {
    free(identity);
    return next(chain, request, response);
  }
  set_context(request, identity, sizeof(*identity), free);
  request->security->authenticated = true;
  return next(chain, request, response);
}

static int flowie_control_runtime_resolve_caller(void *ctx, const Req *request,
                                                 flowie_control_management_caller_t *caller_out) {
  const flowie_control_management_session_identity_t *identity;
  (void)ctx;
  if (!request || !caller_out || caller_out->size < sizeof(*caller_out)) return SALTS_EINVAL;
  identity = (const flowie_control_management_session_identity_t *)get_context((Req *)request);
  if (!identity || identity->size < sizeof(*identity)) return SALTS_EPERM;
  caller_out->domain_id = identity->domain_id;
  caller_out->actor = identity->principal_id;
  caller_out->permissions = identity->permissions;
  return SALTS_OK;
}

static int flowie_control_runtime_resolve_session(
    void *ctx, const Req *request, flowie_control_management_caller_t *caller_out,
    char csrf_token_out[FLOWIE_CONTROL_DASHBOARD_CSRF_SIZE + 1u]) {
  const flowie_control_management_session_identity_t *identity;
  (void)ctx;
  if (csrf_token_out) csrf_token_out[0] = '\0';
  if (!request || !caller_out || caller_out->size < sizeof(*caller_out) || !csrf_token_out)
    return SALTS_EINVAL;
  identity = (const flowie_control_management_session_identity_t *)get_context((Req *)request);
  if (!identity || identity->size < sizeof(*identity)) return SALTS_EPERM;
  caller_out->domain_id = identity->domain_id;
  caller_out->actor = identity->principal_id;
  caller_out->permissions = identity->permissions;
  memcpy(csrf_token_out, identity->csrf, sizeof(identity->csrf));
  return SALTS_OK;
}

static int
flowie_control_runtime_login(void *ctx, const char *domain_id, const char *principal_id,
                             const uint8_t *secret, size_t secret_size, const char *remote_address,
                             char token_out[FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u]) {
  flowie_control_runtime_t *runtime = (flowie_control_runtime_t *)ctx;
  if (!runtime) return SALTS_EINVAL;
  return flowie_control_management_session_login(runtime->management_sessions, domain_id,
                                                 principal_id, secret, secret_size, remote_address,
                                                 token_out);
}

static int flowie_control_runtime_logout(void *ctx, const char *token) {
  flowie_control_runtime_t *runtime = (flowie_control_runtime_t *)ctx;
  return runtime ? flowie_control_management_session_revoke(runtime->management_sessions, token)
                 : SALTS_EINVAL;
}

static int flowie_control_runtime_policy_version(void *ctx, const char *domain_id,
                                                 uint64_t *policy_version_out) {
  flowie_control_runtime_t *runtime = (flowie_control_runtime_t *)ctx;
  flowie_control_policy_status_t status = FLOWIE_CONTROL_POLICY_STATUS_INIT;
  int rc;
  if (policy_version_out) *policy_version_out = 0u;
  if (!runtime || flowie_control_repository_validate(runtime->repository) != SALTS_OK ||
      !domain_id || !policy_version_out)
    return SALTS_EINVAL;
  rc = runtime->repository->policy->status(runtime->repository->ctx, domain_id, &status);
  if (rc == SALTS_OK) *policy_version_out = status.policy_version;
  return rc;
}

static int flowie_control_runtime_secret_acquire(void *ctx, const char *reference,
                                                 flowie_security_secret_lease_t *lease_out) {
  const char *value = NULL;
  int rc;
  (void)ctx;
  if (!lease_out || lease_out->size < sizeof(*lease_out)) return SALTS_EINVAL;
  rc = flowie_control_runtime_env_secret(reference, &value);
  if (rc != SALTS_OK) return rc;
  *lease_out = (flowie_security_secret_lease_t)FLOWIE_SECURITY_SECRET_LEASE_INIT;
  lease_out->bytes = (const uint8_t *)value;
  lease_out->byte_count = strlen(value);
  lease_out->provider_lease = (void *)value;
  return SALTS_OK;
}

static void flowie_control_runtime_secret_release(void *ctx,
                                                  flowie_security_secret_lease_t *lease) {
  (void)ctx;
  (void)lease;
}

static flowie_security_key_provider_t
flowie_control_runtime_key_provider(flowie_control_runtime_t *runtime) {
  flowie_security_key_provider_t provider = FLOWIE_SECURITY_KEY_PROVIDER_INIT;
  provider.ctx = runtime;
  provider.acquire = flowie_control_runtime_secret_acquire;
  provider.release = flowie_control_runtime_secret_release;
  return provider;
}

#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
static void flowie_control_runtime_external_configs(
    const flowie_control_config_t *config, const flowie_security_key_provider_t *key_provider,
    flowie_control_external_https_authenticator_config_t *authenticator_out,
    flowie_control_external_subject_mapper_config_t *mapper_out) {
  const flowie_control_config_external_https_t *external = &config->auth.external_https;
  *authenticator_out = (flowie_control_external_https_authenticator_config_t)
      FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_CONFIG_INIT;
  authenticator_out->url = external->url;
  authenticator_out->method = config->auth.method;
  authenticator_out->service_token_ref = external->service_token_ref;
  authenticator_out->timeout_ms = external->timeout_ms;
  authenticator_out->max_response_size = external->max_response_size;
  authenticator_out->max_in_flight = external->max_in_flight;
  authenticator_out->key_provider = *key_provider;
  authenticator_out->tls.ca_file = external->tls.ca_file[0] ? external->tls.ca_file : NULL;
  authenticator_out->tls.client_cert_file =
      external->tls.client_cert_file[0] ? external->tls.client_cert_file : NULL;
  authenticator_out->tls.client_key_file =
      external->tls.client_key_file[0] ? external->tls.client_key_file : NULL;
  authenticator_out->tls.client_key_password_ref =
      external->tls.client_key_password_ref[0] ? external->tls.client_key_password_ref : NULL;
  *mapper_out = (flowie_control_external_subject_mapper_config_t)
      FLOWIE_CONTROL_EXTERNAL_SUBJECT_MAPPER_CONFIG_INIT;
  mapper_out->trusted_issuer = external->trusted_issuer;
  mapper_out->subject_type = external->subject_type;
}

static int flowie_control_runtime_validate_external_https(const flowie_control_config_t *config) {
  flowie_security_key_provider_t key_provider = flowie_control_runtime_key_provider(NULL);
  flowie_control_external_https_authenticator_config_t authenticator_config;
  flowie_control_external_subject_mapper_config_t mapper_config;
  flowie_control_external_https_authenticator_t *authenticator = NULL;
  flowie_control_external_subject_mapper_t *mapper = NULL;
  int rc;
  if (!config->auth.external_https.enabled) return SALTS_OK;
  flowie_control_runtime_external_configs(config, &key_provider, &authenticator_config,
                                          &mapper_config);
  rc = flowie_control_external_https_authenticator_create(&authenticator_config, &authenticator);
  if (rc == SALTS_OK) rc = flowie_control_external_subject_mapper_create(&mapper_config, &mapper);
  flowie_control_external_subject_mapper_destroy(mapper);
  flowie_control_external_https_authenticator_destroy(authenticator);
  return rc;
}

static int
flowie_control_runtime_create_external_https(flowie_control_runtime_t *runtime,
                                             flowie_control_auth_service_config_t *service_config) {
  flowie_security_key_provider_t key_provider = flowie_control_runtime_key_provider(runtime);
  flowie_control_external_https_authenticator_config_t authenticator_config;
  flowie_control_external_subject_mapper_config_t mapper_config;
  int rc;
  if (!runtime->config.auth.external_https.enabled) return SALTS_OK;
  if (runtime->external_https_authenticator && runtime->external_subject_mapper) {
    service_config->external_authenticator = flowie_control_external_https_authenticator_interface(
        runtime->external_https_authenticator);
    service_config->external_identity_mapper =
        flowie_control_external_subject_mapper_interface(runtime->external_subject_mapper);
    return SALTS_OK;
  }
  flowie_control_runtime_external_configs(&runtime->config, &key_provider, &authenticator_config,
                                          &mapper_config);
  rc = flowie_control_external_https_authenticator_create(&authenticator_config,
                                                          &runtime->external_https_authenticator);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_external_subject_mapper_create(&mapper_config,
                                                     &runtime->external_subject_mapper);
  if (rc != SALTS_OK) return rc;
  service_config->external_authenticator =
      flowie_control_external_https_authenticator_interface(runtime->external_https_authenticator);
  service_config->external_identity_mapper =
      flowie_control_external_subject_mapper_interface(runtime->external_subject_mapper);
  return SALTS_OK;
}
#endif

#if defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
static void flowie_control_runtime_jwt_jwks_configs(
    const flowie_control_config_t *config,
    flowie_control_jwt_jwks_authenticator_config_t *authenticator_out,
    flowie_control_external_subject_mapper_config_t *mapper_out) {
  const flowie_control_config_jwt_jwks_t *jwt = &config->auth.jwt_jwks;
  *authenticator_out = (flowie_control_jwt_jwks_authenticator_config_t)
      FLOWIE_CONTROL_JWT_JWKS_AUTHENTICATOR_CONFIG_INIT;
  authenticator_out->url = jwt->url;
  authenticator_out->method = config->auth.method;
  authenticator_out->trusted_issuer = jwt->trusted_issuer;
  authenticator_out->audience = jwt->audience;
  authenticator_out->subject_type = jwt->subject_type;
  authenticator_out->algorithm = jwt->algorithm;
  authenticator_out->ca_file = jwt->ca_file[0] ? jwt->ca_file : NULL;
  authenticator_out->timeout_ms = jwt->timeout_ms;
  authenticator_out->max_response_size = jwt->max_response_size;
  authenticator_out->max_keys = jwt->max_keys;
  authenticator_out->max_token_size = jwt->max_token_size;
  authenticator_out->refresh_interval_seconds = jwt->refresh_interval_seconds;
  authenticator_out->clock_skew_seconds = jwt->clock_skew_seconds;
  authenticator_out->executor_workers = jwt->executor_workers;
  authenticator_out->executor_queue_capacity = jwt->executor_queue_capacity;
  authenticator_out->executor_deadline_ms = jwt->executor_deadline_ms;
  *mapper_out = (flowie_control_external_subject_mapper_config_t)
      FLOWIE_CONTROL_EXTERNAL_SUBJECT_MAPPER_CONFIG_INIT;
  mapper_out->trusted_issuer = jwt->trusted_issuer;
  mapper_out->subject_type = jwt->subject_type;
}

static int flowie_control_runtime_validate_jwt_jwks(const flowie_control_config_t *config) {
  flowie_control_jwt_jwks_authenticator_config_t authenticator_config;
  flowie_control_external_subject_mapper_config_t mapper_config;
  flowie_control_jwt_jwks_authenticator_t *authenticator = NULL;
  flowie_control_external_subject_mapper_t *mapper = NULL;
  int rc;
  if (!config->auth.jwt_jwks.enabled) return SALTS_OK;
  flowie_control_runtime_jwt_jwks_configs(config, &authenticator_config, &mapper_config);
  rc = flowie_control_jwt_jwks_authenticator_create(&authenticator_config, &authenticator);
  if (rc == SALTS_OK) rc = flowie_control_external_subject_mapper_create(&mapper_config, &mapper);
  flowie_control_external_subject_mapper_destroy(mapper);
  flowie_control_jwt_jwks_authenticator_destroy(authenticator);
  return rc;
}

static int
flowie_control_runtime_create_jwt_jwks(flowie_control_runtime_t *runtime,
                                       flowie_control_auth_service_config_t *service_config) {
  flowie_control_jwt_jwks_authenticator_config_t authenticator_config;
  flowie_control_external_subject_mapper_config_t mapper_config;
  int rc;
  if (!runtime->config.auth.jwt_jwks.enabled) return SALTS_OK;
  if (runtime->jwt_jwks_authenticator && runtime->external_subject_mapper) {
    service_config->external_authenticator =
        flowie_control_jwt_jwks_authenticator_interface(runtime->jwt_jwks_authenticator);
    service_config->external_identity_mapper =
        flowie_control_external_subject_mapper_interface(runtime->external_subject_mapper);
    return SALTS_OK;
  }
  flowie_control_runtime_jwt_jwks_configs(&runtime->config, &authenticator_config, &mapper_config);
  rc = flowie_control_jwt_jwks_authenticator_create(&authenticator_config,
                                                    &runtime->jwt_jwks_authenticator);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_external_subject_mapper_create(&mapper_config,
                                                     &runtime->external_subject_mapper);
  if (rc != SALTS_OK) return rc;
  service_config->external_authenticator =
      flowie_control_jwt_jwks_authenticator_interface(runtime->jwt_jwks_authenticator);
  service_config->external_identity_mapper =
      flowie_control_external_subject_mapper_interface(runtime->external_subject_mapper);
  return SALTS_OK;
}
#endif

static int flowie_control_runtime_create_management_sessions(flowie_control_runtime_t *runtime) {
  flowie_control_auth_service_config_t auth_config = FLOWIE_CONTROL_AUTH_SERVICE_CONFIG_INIT;
  flowie_control_management_session_config_t session_config =
      FLOWIE_CONTROL_MANAGEMENT_SESSION_CONFIG_INIT;
  int rc;
  if (!runtime || !runtime->repository) return SALTS_EINVAL;
  auth_config.repository = runtime->repository;
  auth_config.method = runtime->config.auth.enabled ? runtime->config.auth.method : "password";
  auth_config.principal_ttl_seconds =
      runtime->config.management.session_ttl_seconds > FLOWIE_CONTROL_AUTH_MAX_PRINCIPAL_TTL_SECONDS
          ? FLOWIE_CONTROL_AUTH_MAX_PRINCIPAL_TTL_SECONDS
          : runtime->config.management.session_ttl_seconds;
  auth_config.credential_cache.capacity =
      runtime->config.auth.enabled ? runtime->config.auth.credential_cache_capacity : 4096u;
  auth_config.credential_cache.ttl_ms =
      (runtime->config.auth.enabled ? runtime->config.auth.credential_cache_ttl_seconds : 60u) *
      1000u;
  auth_config.principal_cache.capacity = auth_config.credential_cache.capacity;
  auth_config.principal_cache.ttl_ms = auth_config.credential_cache.ttl_ms;
  auth_config.policy_version.ctx = runtime;
  auth_config.policy_version.current = flowie_control_runtime_policy_version;
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
  rc = flowie_control_runtime_create_external_https(runtime, &auth_config);
  if (rc != SALTS_OK) return rc;
#else
  if (runtime->config.auth.external_https.enabled) return SALTS_ENOTSUP;
#endif
#if defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
  rc = flowie_control_runtime_create_jwt_jwks(runtime, &auth_config);
  if (rc != SALTS_OK) return rc;
#else
  if (runtime->config.auth.jwt_jwks.enabled) return SALTS_ENOTSUP;
#endif
  rc = flowie_control_auth_service_create(&auth_config, &runtime->management_auth_service);
  if (rc != SALTS_OK) return rc;
  session_config.repository = runtime->repository;
  session_config.auth_service = runtime->management_auth_service;
  session_config.method = auth_config.method;
  session_config.capacity = runtime->config.management.session_capacity;
  session_config.max_sessions_per_principal =
      runtime->config.management.session_max_sessions_per_principal;
  session_config.ttl_seconds = runtime->config.management.session_ttl_seconds;
  session_config.clock = flowie_control_runtime_clock;
  session_config.clock_ctx = runtime;
  return flowie_control_management_session_store_create(&session_config,
                                                        &runtime->management_sessions);
}

static int flowie_control_runtime_create_auth(flowie_control_runtime_t *runtime) {
  flowie_control_service_credential_config_t credential_config =
      FLOWIE_CONTROL_SERVICE_CREDENTIAL_CONFIG_INIT;
  flowie_control_auth_service_config_t service_config = FLOWIE_CONTROL_AUTH_SERVICE_CONFIG_INIT;
  flowie_control_auth_iris_adapter_config_t adapter_config =
      FLOWIE_CONTROL_AUTH_IRIS_ADAPTER_CONFIG_INIT;
  flowie_control_auth_iris_endpoint_config_t endpoint_config =
      FLOWIE_CONTROL_AUTH_IRIS_ENDPOINT_CONFIG_INIT;
  flowie_control_acl_iris_endpoint_config_t acl_config =
      FLOWIE_CONTROL_ACL_IRIS_ENDPOINT_CONFIG_INIT;
  int rc;
  if (!runtime->config.auth.enabled) return SALTS_OK;
  service_config.repository = runtime->repository;
  service_config.method = runtime->config.auth.method;
  service_config.principal_ttl_seconds = runtime->config.auth.principal_ttl_seconds;
  service_config.credential_cache.capacity = runtime->config.auth.credential_cache_capacity;
  service_config.credential_cache.ttl_ms =
      runtime->config.auth.credential_cache_ttl_seconds * 1000u;
  service_config.principal_cache.capacity = runtime->config.auth.credential_cache_capacity;
  service_config.principal_cache.ttl_ms = runtime->config.auth.credential_cache_ttl_seconds * 1000u;
  service_config.policy_version.ctx = runtime;
  service_config.policy_version.current = flowie_control_runtime_policy_version;
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
  rc = flowie_control_runtime_create_external_https(runtime, &service_config);
  if (rc != SALTS_OK) return rc;
#else
  if (runtime->config.auth.external_https.enabled) return SALTS_ENOTSUP;
#endif
#if defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
  rc = flowie_control_runtime_create_jwt_jwks(runtime, &service_config);
  if (rc != SALTS_OK) return rc;
#else
  if (runtime->config.auth.jwt_jwks.enabled) return SALTS_ENOTSUP;
#endif
  rc = flowie_control_auth_service_create(&service_config, &runtime->auth_service);
  if (rc != SALTS_OK) return rc;
  credential_config.listener_id = runtime->config.auth.listener_id;
  credential_config.repository = runtime->repository;
  rc = flowie_control_service_credential_resolver_create(&credential_config,
                                                         &runtime->service_credentials);
  if (rc != SALTS_OK) return rc;
  adapter_config.service = runtime->auth_service;
  rc = flowie_control_auth_iris_adapter_create(&adapter_config, &runtime->auth_adapter);
  if (rc != SALTS_OK) return rc;
  endpoint_config.adapter = runtime->auth_adapter;
  endpoint_config.service_credentials = runtime->service_credentials;
  endpoint_config.max_request_body_size = FLOWIE_CONTROL_AUTH_ENDPOINT_BODY_MAX;
  endpoint_config.local_executor_enabled =
      flowie_control_runtime_external_auth_enabled(&runtime->config) ? 0 : 1;
  endpoint_config.local_executor_workers = runtime->config.auth.local_executor.workers;
  endpoint_config.local_executor_queue_capacity =
      runtime->config.auth.local_executor.queue_capacity;
  endpoint_config.local_executor_deadline_ms = runtime->config.auth.local_executor.deadline_ms;
  rc = flowie_control_auth_iris_endpoint_create(&endpoint_config, &runtime->auth_endpoint);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_auth_iris_endpoint_register(runtime->auth_endpoint, runtime->app);
  if (rc != SALTS_OK) return rc;
  acl_config.repository = runtime->repository;
  acl_config.service_credentials = runtime->service_credentials;
  rc = flowie_control_acl_iris_endpoint_create(&acl_config, &runtime->acl_endpoint);
  if (rc != SALTS_OK) return rc;
  return flowie_control_acl_iris_endpoint_register(runtime->acl_endpoint, runtime->app);
}

static int flowie_control_runtime_create_repository(flowie_control_runtime_t *runtime) {
  flowie_control_store_config_t store_config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
  orm_config_t database;
  orm_option_t options[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX];
  int rc;
  if (!runtime) return SALTS_EINVAL;
  rc = flowie_control_runtime_turbodb_config(&runtime->config, &database, options);
  if (rc != SALTS_OK) return rc;
  store_config.database = &database;
  rc = flowie_control_store_open(&store_config, &runtime->store);
  if (rc != SALTS_OK) return rc;
  runtime->repository = flowie_control_store_repository(runtime->store);
  return flowie_control_repository_validate(runtime->repository);
}

int flowie_control_runtime_create(const flowie_control_config_t *config,
                                  flowie_control_runtime_t **out) {
  flowie_control_runtime_t *runtime = NULL;
  flowie_control_management_service_config_t management_config =
      FLOWIE_CONTROL_MANAGEMENT_SERVICE_CONFIG_INIT;
  flowie_control_management_rpc_server_config_t rpc_server_config =
      FLOWIE_CONTROL_MANAGEMENT_RPC_SERVER_CONFIG_INIT;
  flowie_control_dashboard_config_t dashboard_config = FLOWIE_CONTROL_DASHBOARD_CONFIG_INIT;
  flowie_control_http_limits_t limits;
  rpc_config_t rpc_config = RPC_DEFAULT_CONFIG();
  size_t worker_count;
  size_t worker_queue_capacity;
  int rc;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) ||
      config->version != FLOWIE_CONTROL_CONFIG_VERSION || !out)
    return SALTS_EINVAL;
  rc = flowie_control_runtime_validate(config);
  if (rc != SALTS_OK) return rc;
  runtime = (flowie_control_runtime_t *)calloc(1u, sizeof(*runtime));
  if (!runtime) return SALTS_ENOMEM;
  runtime->config = *config;
  rc = flowie_control_runtime_create_repository(runtime);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_control_bootstrap_apply(runtime->repository, &runtime->config.bootstrap,
                                      FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD,
                                      sizeof(FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD) - 1u,
                                      flowie_control_runtime_clock(NULL));
  if (rc != SALTS_OK) goto fail;
  rc = flowie_control_runtime_create_management_sessions(runtime);
  if (rc != SALTS_OK) goto fail;
  management_config.repository = runtime->repository;
  rc = flowie_control_management_service_create(&management_config, &runtime->management_service);
  if (rc != SALTS_OK) goto fail;
  runtime->app = flowie_control_http_app_create();
  if (!runtime->app) {
    rc = SALTS_ENOMEM;
    goto fail;
  }
  if (flowie_control_http_app_bind_context(runtime->app,
                                           FLOWIE_CONTROL_RUNTIME_SESSION_CONTEXT,
                                           runtime) != SALTS_OK) {
    rc = SALTS_EBUSY;
    goto fail;
  }
  rc = flowie_control_http_app_use(runtime->app, flowie_control_runtime_session_middleware);
  if (rc != SALTS_OK) goto fail;
  limits = (flowie_control_http_limits_t){
      runtime->config.listener.limits.max_header_name_length,
      runtime->config.listener.limits.max_header_value_length,
      runtime->config.listener.limits.max_url_length,
      runtime->config.listener.limits.max_request_body_size,
      runtime->config.listener.limits.max_headers_count};
  worker_count = runtime->config.management.login_executor_workers;
  worker_queue_capacity = runtime->config.management.login_executor_queue_capacity;
  if (runtime->config.auth.enabled && runtime->config.auth.local_executor.workers > worker_count)
    worker_count = runtime->config.auth.local_executor.workers;
  if (runtime->config.auth.enabled &&
      runtime->config.auth.local_executor.queue_capacity > worker_queue_capacity)
    worker_queue_capacity = runtime->config.auth.local_executor.queue_capacity;
  rc = flowie_control_http_app_configure(runtime->app, &limits, worker_count,
                                         worker_queue_capacity);
  if (rc != SALTS_OK) goto fail;
  rpc_config.endpoint = runtime->config.management.rpc_path;
  rpc_config.enable_introspection = 0;
  rpc_config.enable_batch = 0;
  rpc_config.max_batch_size = 1;
  rpc_config.max_request_size = runtime->config.management.rpc_max_request_size;
  rpc_config.max_response_size = FLOWIE_CONTROL_MANAGEMENT_RPC_RESPONSE_MAX;
  runtime->rpc_context = rpc_init(&rpc_config);
  if (!runtime->rpc_context) {
    rc = SALTS_ENOMEM;
    goto fail;
  }
  rpc_server_config.service = runtime->management_service;
  rpc_server_config.rpc_context = runtime->rpc_context;
  rpc_server_config.resolve_caller = flowie_control_runtime_resolve_caller;
  rpc_server_config.resolve_caller_ctx = runtime;
  rpc_server_config.clock = flowie_control_runtime_clock;
  rpc_server_config.external_https_stats = flowie_control_runtime_external_https_stats;
  rpc_server_config.external_https_stats_ctx = runtime;
  rc = flowie_control_management_rpc_server_create(&rpc_server_config, &runtime->management_rpc);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_control_management_rpc_server_bind(runtime->management_rpc, runtime->app);
  if (rc != SALTS_OK) goto fail;
  if (runtime->config.dashboard_enabled) {
    dashboard_config.service = runtime->management_service;
    dashboard_config.resolve_session = flowie_control_runtime_resolve_session;
    dashboard_config.resolve_session_ctx = runtime;
    dashboard_config.clock = flowie_control_runtime_clock;
    dashboard_config.clock_ctx = runtime;
    dashboard_config.login = flowie_control_runtime_login;
    dashboard_config.logout = flowie_control_runtime_logout;
    dashboard_config.session_ctx = runtime;
    dashboard_config.session_ttl_seconds = runtime->config.management.session_ttl_seconds;
    dashboard_config.login_executor_enabled =
        flowie_control_runtime_external_auth_enabled(&runtime->config) ? 0 : 1;
    dashboard_config.login_executor_workers = runtime->config.management.login_executor_workers;
    dashboard_config.login_executor_queue_capacity =
        runtime->config.management.login_executor_queue_capacity;
    dashboard_config.login_executor_deadline_ms =
        runtime->config.management.login_executor_deadline_ms;
    dashboard_config.rpc_path = runtime->config.management.rpc_path;
    rc = flowie_control_dashboard_create(&dashboard_config, &runtime->dashboard);
    if (rc != SALTS_OK) goto fail;
    rc = flowie_control_dashboard_bind(runtime->dashboard, runtime->app);
    if (rc != SALTS_OK) goto fail;
  }
  rc = flowie_control_runtime_create_auth(runtime);
  if (rc != SALTS_OK) goto fail;
  *out = runtime;
  return SALTS_OK;

fail: {
  int cleanup_rc = flowie_control_runtime_destroy(runtime);
  if (cleanup_rc != SALTS_OK) return cleanup_rc;
}
  return rc;
}

int flowie_control_runtime_start(flowie_control_runtime_t *runtime) {
  flowie_control_http_tls_config_t tls = FLOWIE_CONTROL_HTTP_TLS_CONFIG_INIT;
  int rc;
  if (!runtime || !runtime->app || runtime->listener_started) return SALTS_EINVAL;
  rc = flowie_control_runtime_tls_config(&runtime->config, &tls);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_http_app_start_tls(runtime->app, runtime->config.listener.host,
                                         runtime->config.listener.port, &tls);
  if (rc == SALTS_OK) runtime->listener_started = 1;
  return rc;
}

int flowie_control_runtime_stop(flowie_control_runtime_t *runtime) {
  int rc;
  if (!runtime) return SALTS_EINVAL;
  if (!runtime->listener_started) return SALTS_OK;
  rc = flowie_control_http_app_stop(runtime->app, 0u);
  if (rc == SALTS_OK) runtime->listener_started = 0;
  return rc;
}

int flowie_control_runtime_run(flowie_control_runtime_t *runtime) {
  void (*previous_int)(int);
  void (*previous_term)(int);
  int rc;
  if (!runtime || runtime->listener_started) return SALTS_EINVAL;
  flowie_control_runtime_stop_requested = 0;
  previous_int = signal(SIGINT, flowie_control_runtime_signal);
  if (previous_int == SIG_ERR) return SALTS_EIO;
  previous_term = signal(SIGTERM, flowie_control_runtime_signal);
  if (previous_term == SIG_ERR) {
    (void)signal(SIGINT, previous_int);
    return SALTS_EIO;
  }
  rc = flowie_control_runtime_start(runtime);
  while (rc == SALTS_OK && !flowie_control_runtime_stop_requested) salts_sleep_ms(100u);
  if (rc == SALTS_OK) rc = flowie_control_runtime_stop(runtime);
  (void)signal(SIGTERM, previous_term);
  (void)signal(SIGINT, previous_int);
  return rc;
}

int flowie_control_runtime_destroy(flowie_control_runtime_t *runtime) {
  int rc;
  if (!runtime) return SALTS_OK;
  rc = flowie_control_runtime_stop(runtime);
  if (rc != SALTS_OK) return rc;
  flowie_control_acl_iris_endpoint_destroy(runtime->acl_endpoint);
  runtime->acl_endpoint = NULL;
  flowie_control_auth_iris_endpoint_destroy(runtime->auth_endpoint);
  runtime->auth_endpoint = NULL;
  flowie_control_auth_iris_adapter_destroy(runtime->auth_adapter);
  runtime->auth_adapter = NULL;
  flowie_control_service_credential_resolver_destroy(runtime->service_credentials);
  runtime->service_credentials = NULL;
  flowie_control_auth_service_destroy(runtime->auth_service);
  runtime->auth_service = NULL;
  flowie_control_dashboard_destroy(runtime->dashboard);
  runtime->dashboard = NULL;
  flowie_control_management_rpc_server_destroy(runtime->management_rpc);
  runtime->management_rpc = NULL;
  if (runtime->app)
    (void)flowie_control_http_app_unbind_context(runtime->app,
                                                 FLOWIE_CONTROL_RUNTIME_SESSION_CONTEXT,
                                                 runtime);
  flowie_control_http_app_destroy(runtime->app);
  runtime->app = NULL;
  rpc_destroy(runtime->rpc_context);
  runtime->rpc_context = NULL;
  flowie_control_management_session_store_destroy(runtime->management_sessions);
  runtime->management_sessions = NULL;
  flowie_control_auth_service_destroy(runtime->management_auth_service);
  runtime->management_auth_service = NULL;
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH) || defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
  flowie_control_external_subject_mapper_destroy(runtime->external_subject_mapper);
  runtime->external_subject_mapper = NULL;
#endif
#if defined(FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH)
  flowie_control_external_https_authenticator_destroy(runtime->external_https_authenticator);
  runtime->external_https_authenticator = NULL;
#endif
#if defined(FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH)
  flowie_control_jwt_jwks_authenticator_destroy(runtime->jwt_jwks_authenticator);
  runtime->jwt_jwks_authenticator = NULL;
#endif
  flowie_control_management_service_destroy(runtime->management_service);
  runtime->management_service = NULL;
  rc = SALTS_OK;
  flowie_control_store_destroy(runtime->store);
  runtime->store = NULL;
  runtime->repository = NULL;
  crypto_wipe(runtime, sizeof(*runtime));
  free(runtime);
  return rc;
}
