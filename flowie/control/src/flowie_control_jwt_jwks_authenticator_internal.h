#ifndef FLOWIE_CONTROL_JWT_JWKS_AUTHENTICATOR_INTERNAL_H
#define FLOWIE_CONTROL_JWT_JWKS_AUTHENTICATOR_INTERNAL_H

#include "flowie_control_external_authenticator_internal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_CONTROL_JWT_JWKS_URL_MAX 2047u
#define FLOWIE_CONTROL_JWT_JWKS_ALGORITHM_MAX 15u
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_TIMEOUT_MS 3000u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_TIMEOUT_MS 30000u
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_RESPONSE_SIZE 65536u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_RESPONSE_SIZE (1024u * 1024u)
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_MAX_KEYS 16u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_KEYS 64u
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_TOKEN_SIZE 4096u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_TOKEN_SIZE 16384u
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_REFRESH_SECONDS 300u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_REFRESH_SECONDS 86400u
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_CLOCK_SKEW_SECONDS 30u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_CLOCK_SKEW_SECONDS 300u
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_WORKERS 4u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_WORKERS 64u
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_QUEUE_CAPACITY 128u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_QUEUE_CAPACITY 4096u
#define FLOWIE_CONTROL_JWT_JWKS_DEFAULT_DEADLINE_MS 10000u
#define FLOWIE_CONTROL_JWT_JWKS_MAX_DEADLINE_MS 60000u

typedef struct flowie_control_jwt_jwks_authenticator_s flowie_control_jwt_jwks_authenticator_t;

typedef uint64_t (*flowie_control_jwt_jwks_clock_fn)(void *ctx);

typedef struct flowie_control_jwt_jwks_authenticator_config_s {
  size_t size;
  const char *url;
  const char *method;
  const char *trusted_issuer;
  const char *audience;
  const char *subject_type;
  const char *algorithm;
  const char *ca_file;
  uint32_t timeout_ms;
  size_t max_response_size;
  uint32_t max_keys;
  size_t max_token_size;
  uint64_t refresh_interval_seconds;
  uint32_t clock_skew_seconds;
  uint32_t executor_workers;
  size_t executor_queue_capacity;
  uint32_t executor_deadline_ms;
  flowie_control_jwt_jwks_clock_fn clock_seconds;
  void *clock_ctx;
} flowie_control_jwt_jwks_authenticator_config_t;

#define FLOWIE_CONTROL_JWT_JWKS_AUTHENTICATOR_CONFIG_INIT                                          \
  {sizeof(flowie_control_jwt_jwks_authenticator_config_t),                                         \
   NULL,                                                                                           \
   NULL,                                                                                           \
   NULL,                                                                                           \
   NULL,                                                                                           \
   NULL,                                                                                           \
   "RS256",                                                                                        \
   NULL,                                                                                           \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_TIMEOUT_MS,                                                     \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_RESPONSE_SIZE,                                                  \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_MAX_KEYS,                                                       \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_TOKEN_SIZE,                                                     \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_REFRESH_SECONDS,                                                \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_CLOCK_SKEW_SECONDS,                                             \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_WORKERS,                                                        \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_QUEUE_CAPACITY,                                                 \
   FLOWIE_CONTROL_JWT_JWKS_DEFAULT_DEADLINE_MS,                                                    \
   NULL,                                                                                           \
   NULL}

int flowie_control_jwt_jwks_authenticator_create(
    const flowie_control_jwt_jwks_authenticator_config_t *config,
    flowie_control_jwt_jwks_authenticator_t **out);
void flowie_control_jwt_jwks_authenticator_destroy(
    flowie_control_jwt_jwks_authenticator_t *authenticator);

const flowie_control_external_authenticator_t *flowie_control_jwt_jwks_authenticator_interface(
    const flowie_control_jwt_jwks_authenticator_t *authenticator);

/**
 * Validate and install one immutable JWKS snapshot.
 *
 * Parsing performs bounded JSON and public-key validation and may be CPU intensive. Production
 * callers must execute it through the bounded CHTTP worker path, not on the listener owner thread.
 * This direct entry exists for startup validation and focused contract tests.
 */
int flowie_control_jwt_jwks_authenticator_install(
    flowie_control_jwt_jwks_authenticator_t *authenticator, const char *jwks_json, size_t jwks_size,
    uint64_t valid_until);

/** Direct verifier for worker execution and focused tests. */
int flowie_control_jwt_jwks_authenticator_verify_token(
    flowie_control_jwt_jwks_authenticator_t *authenticator,
    const flowie_control_external_auth_request_t *request, uint64_t now,
    flowie_control_external_auth_assertion_t *assertion_out);

#ifdef __cplusplus
}
#endif

#endif
