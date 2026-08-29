#include "flowie_control_management_session_internal.h"

#include "flowie_control_credential_internal.h"

#include "monocypher.h"
#include "platform.h"
#include "turbo_error.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FLOWIE_CONTROL_MANAGEMENT_SESSION_RANDOM_SIZE 32u
#define FLOWIE_CONTROL_MANAGEMENT_SESSION_SCOPE "management-session"

struct flowie_control_management_session_store_s {
  flowie_control_repository_t repository;
  flowie_control_auth_service_t *auth_service;
  char method[FLOWIE_SECURITY_TYPE_MAX + 1u];
  size_t capacity;
  size_t max_sessions_per_principal;
  uint64_t ttl_seconds;
  flowie_control_management_session_clock_fn clock;
  void *clock_ctx;
};


static uint64_t flowie_control_management_session_default_clock(void *ctx) {
  (void)ctx;
  return turbo_realtime_ms() / 1000u;
}

static int flowie_control_management_session_text_valid(const char *value, size_t maximum) {
  size_t size;
  if (!value || maximum == 0u) return 0;
  size = strnlen(value, maximum + 1u);
  if (size == 0u || size > maximum) return 0;
  for (size_t index = 0u; index < size; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (byte < 0x20u || byte == 0x7fu) return 0;
  }
  return 1;
}

static void flowie_control_management_session_hex(
    const uint8_t input[FLOWIE_CONTROL_MANAGEMENT_SESSION_RANDOM_SIZE],
    char output[FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t index = 0u; index < FLOWIE_CONTROL_MANAGEMENT_SESSION_RANDOM_SIZE; ++index) {
    output[index * 2u] = hex[input[index] >> 4u];
    output[index * 2u + 1u] = hex[input[index] & 0x0fu];
  }
  output[FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE] = '\0';
}

static int flowie_control_management_session_token_valid(const char *token) {
  if (!token ||
      strnlen(token, FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u) !=
          FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE)
    return 0;
  for (size_t index = 0u; index < FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE; ++index) {
    const char byte = token[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return 0;
  }
  return 1;
}

static void flowie_control_management_session_digest(
    const char *token,
    uint8_t digest[FLOWIE_CONTROL_MANAGEMENT_SESSION_DIGEST_SIZE]) {
  crypto_blake2b(digest, FLOWIE_CONTROL_MANAGEMENT_SESSION_DIGEST_SIZE,
                 (const uint8_t *)token, FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE);
}

int flowie_control_management_session_store_create(
    const flowie_control_management_session_config_t *config,
    flowie_control_management_session_store_t **out) {
  flowie_control_management_session_store_t *store = NULL;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) ||
      flowie_control_repository_validate(config->repository) != TURBO_OK ||
      !config->auth_service ||
      !flowie_control_management_session_text_valid(config->method,
                                                     FLOWIE_SECURITY_TYPE_MAX) ||
      config->capacity == 0u ||
      config->capacity > FLOWIE_CONTROL_MANAGEMENT_SESSION_MAX_CAPACITY ||
      config->max_sessions_per_principal == 0u ||
      config->max_sessions_per_principal > FLOWIE_CONTROL_MANAGEMENT_SESSION_MAX_PER_PRINCIPAL ||
      config->ttl_seconds < 60u ||
      config->ttl_seconds > FLOWIE_CONTROL_MANAGEMENT_SESSION_MAX_TTL_SECONDS || !out)
    return TURBO_EINVAL;
  store = (flowie_control_management_session_store_t *)calloc(1u, sizeof(*store));
  if (!store) return TURBO_ENOMEM;
  store->repository = *config->repository;
  store->auth_service = config->auth_service;
  store->capacity = config->capacity;
  store->max_sessions_per_principal = config->max_sessions_per_principal;
  store->ttl_seconds = config->ttl_seconds;
  store->clock =
      config->clock ? config->clock : flowie_control_management_session_default_clock;
  store->clock_ctx = config->clock_ctx;
  memcpy(store->method, config->method, strlen(config->method) + 1u);
  *out = store;
  return TURBO_OK;
}

void flowie_control_management_session_store_destroy(
  flowie_control_management_session_store_t *store) {
  if (!store) return;
  flowie_control_credential_wipe(store, sizeof(*store));
  free(store);
}

static int flowie_control_management_session_authenticate(
    flowie_control_management_session_store_t *store, const char *domain_id,
    const char *presented_identity, const uint8_t *secret, size_t secret_size,
    const char *remote_address, flowie_control_management_caller_t *caller_out,
    char caller_domain_out[FLOWIE_SECURITY_ID_MAX + 1u],
    char caller_actor_out[FLOWIE_SECURITY_ID_MAX + 1u],
    uint64_t *principal_expires_at_out) {
  flowie_control_authenticate_request_t request = FLOWIE_CONTROL_AUTHENTICATE_REQUEST_INIT;
  flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
  flowie_control_management_caller_t caller = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  int rc;
  if (caller_out && caller_out->size >= sizeof(*caller_out))
    *caller_out = (flowie_control_management_caller_t)FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  if (caller_domain_out) caller_domain_out[0] = '\0';
  if (caller_actor_out) caller_actor_out[0] = '\0';
  if (principal_expires_at_out) *principal_expires_at_out = 0u;
  if (!store || !caller_out || caller_out->size < sizeof(*caller_out) ||
      !caller_domain_out || !caller_actor_out || !principal_expires_at_out ||
      !flowie_control_management_session_text_valid(domain_id,
                                                     FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_management_session_text_valid(presented_identity,
                                                     FLOWIE_SECURITY_ID_MAX) ||
      !secret || secret_size == 0u || secret_size > FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX ||
      !flowie_control_management_session_text_valid(remote_address,
                                                     FLOWIE_CONTROL_AUTH_REMOTE_ADDRESS_MAX))
    return TURBO_EINVAL;
  request.identity = presented_identity;
  request.method = store->method;
  request.secret = secret;
  request.secret_size = secret_size;
  request.protocol = "https";
  request.remote_address = remote_address;
  rc = flowie_control_auth_service_authenticate_root(
      store->auth_service, domain_id, FLOWIE_CONTROL_MANAGEMENT_SESSION_SCOPE, &request, 0,
      NULL, &principal, NULL);
  if (rc != TURBO_OK) goto done;
  rc = flowie_control_management_identity_resolve_principal(
      &store->repository, principal.domain_id, principal.principal_id, &caller);
  if (rc == TURBO_OK) {
    memcpy(caller_domain_out, caller.domain_id, strlen(caller.domain_id) + 1u);
    memcpy(caller_actor_out, caller.actor, strlen(caller.actor) + 1u);
    caller_out->domain_id = caller_domain_out;
    caller_out->actor = caller_actor_out;
    caller_out->permissions = caller.permissions;
    *principal_expires_at_out = principal.expires_at;
  }
done:
  flowie_control_credential_wipe(&principal, sizeof(principal));
  return rc;
}

static int flowie_control_management_session_issue(
    flowie_control_management_session_store_t *store,
    const flowie_control_management_caller_t *caller, uint64_t principal_expires_at,
    char token_out[FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u]) {
  flowie_control_management_session_record_t record =
      FLOWIE_CONTROL_MANAGEMENT_SESSION_RECORD_INIT;
  uint8_t token_random[FLOWIE_CONTROL_MANAGEMENT_SESSION_RANDOM_SIZE] = {0};
  uint8_t csrf_random[FLOWIE_CONTROL_MANAGEMENT_SESSION_RANDOM_SIZE] = {0};
  uint64_t now;
  int rc;
  if (token_out) token_out[0] = '\0';
  if (!store || !caller || caller->size < sizeof(*caller) || !token_out ||
      !flowie_control_management_session_text_valid(caller->domain_id,
                                                     FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_management_session_text_valid(caller->actor,
                                                     FLOWIE_SECURITY_ID_MAX) ||
      caller->permissions == 0u || principal_expires_at == 0u)
    return TURBO_EINVAL;
  now = store->clock(store->clock_ctx);
  if (now == 0u || now > UINT64_MAX - store->ttl_seconds) {
    rc = TURBO_EIO;
    goto done;
  }
  if (principal_expires_at <= now) {
    rc = TURBO_EPERM;
    goto done;
  }
  rc = turbo_secure_random(token_random, sizeof(token_random));
  if (rc == TURBO_OK) rc = turbo_secure_random(csrf_random, sizeof(csrf_random));
  if (rc != TURBO_OK) goto done;
  flowie_control_management_session_hex(token_random, token_out);
  flowie_control_management_session_hex(csrf_random, record.csrf);
  memcpy(record.domain_id, caller->domain_id, strlen(caller->domain_id) + 1u);
  memcpy(record.principal_id, caller->actor, strlen(caller->actor) + 1u);
  record.expires_at = now + store->ttl_seconds;
  if (principal_expires_at < record.expires_at) record.expires_at = principal_expires_at;
  flowie_control_management_session_digest(token_out, record.token_digest);
  rc = store->repository.session->issue(store->repository.ctx, &record, store->capacity,
                                        store->max_sessions_per_principal, now);

done:
  if (rc != TURBO_OK && token_out) token_out[0] = '\0';
  flowie_control_credential_wipe(&record, sizeof(record));
  flowie_control_credential_wipe(token_random, sizeof(token_random));
  flowie_control_credential_wipe(csrf_random, sizeof(csrf_random));
  return rc;
}

int flowie_control_management_session_login(
    flowie_control_management_session_store_t *store, const char *domain_id,
    const char *presented_identity, const uint8_t *secret, size_t secret_size,
    const char *remote_address,
    char token_out[FLOWIE_CONTROL_MANAGEMENT_SESSION_TOKEN_SIZE + 1u]) {
  flowie_control_management_caller_t caller = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  char caller_domain[FLOWIE_SECURITY_ID_MAX + 1u] = {0};
  char caller_actor[FLOWIE_SECURITY_ID_MAX + 1u] = {0};
  uint64_t principal_expires_at = 0u;
  int rc;
  if (token_out) token_out[0] = '\0';
  if (!store || !token_out ||
      !flowie_control_management_session_text_valid(domain_id,
                                                     FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_management_session_text_valid(presented_identity,
                                                     FLOWIE_SECURITY_ID_MAX) ||
      !secret || secret_size == 0u || secret_size > FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX ||
      !flowie_control_management_session_text_valid(remote_address,
                                                     FLOWIE_CONTROL_AUTH_REMOTE_ADDRESS_MAX))
    return TURBO_EINVAL;
  rc = flowie_control_management_session_authenticate(
      store, domain_id, presented_identity, secret, secret_size, remote_address, &caller,
      caller_domain, caller_actor, &principal_expires_at);
  if (rc == TURBO_OK)
    rc = flowie_control_management_session_issue(store, &caller, principal_expires_at, token_out);
  flowie_control_credential_wipe(&caller, sizeof(caller));
  flowie_control_credential_wipe(caller_domain, sizeof(caller_domain));
  flowie_control_credential_wipe(caller_actor, sizeof(caller_actor));
  return rc;
}

int flowie_control_management_session_resolve(
    flowie_control_management_session_store_t *store, const char *token,
    flowie_control_management_session_identity_t *identity_out) {
  flowie_control_management_session_record_t record =
      FLOWIE_CONTROL_MANAGEMENT_SESSION_RECORD_INIT;
  flowie_control_management_caller_t caller = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  flowie_control_management_session_identity_t identity =
      FLOWIE_CONTROL_MANAGEMENT_SESSION_IDENTITY_INIT;
  uint8_t digest[FLOWIE_CONTROL_MANAGEMENT_SESSION_DIGEST_SIZE] = {0};
  uint64_t now;
  int rc;
  if (identity_out && identity_out->size >= sizeof(*identity_out)) *identity_out = identity;
  if (!store || !flowie_control_management_session_token_valid(token) || !identity_out ||
      identity_out->size < sizeof(*identity_out))
    return TURBO_EPERM;
  now = store->clock(store->clock_ctx);
  if (now == 0u) return TURBO_EIO;
  flowie_control_management_session_digest(token, digest);
  rc = store->repository.session->resolve(store->repository.ctx, digest, now, &record);
  if (rc == TURBO_ENOENT) rc = TURBO_EPERM;
  if (rc != TURBO_OK) goto done;
  rc = flowie_control_management_identity_resolve_principal(
      &store->repository, record.domain_id, record.principal_id, &caller);
  if (rc != TURBO_OK) {
    (void)flowie_control_management_session_revoke(store, token);
    goto done;
  }
  memcpy(identity.domain_id, caller.domain_id, strlen(caller.domain_id) + 1u);
  memcpy(identity.principal_id, caller.actor, strlen(caller.actor) + 1u);
  identity.permissions = caller.permissions;
  memcpy(identity.csrf, record.csrf, sizeof(record.csrf));
  *identity_out = identity;

done:
  flowie_control_credential_wipe(&record, sizeof(record));
  flowie_control_credential_wipe(digest, sizeof(digest));
  return rc;
}

int flowie_control_management_session_revoke(
    flowie_control_management_session_store_t *store, const char *token) {
  uint8_t digest[FLOWIE_CONTROL_MANAGEMENT_SESSION_DIGEST_SIZE] = {0};
  int rc = TURBO_ENOENT;
  if (!store || !flowie_control_management_session_token_valid(token)) return TURBO_EPERM;
  flowie_control_management_session_digest(token, digest);
  rc = store->repository.session->revoke(store->repository.ctx, digest);
  flowie_control_credential_wipe(digest, sizeof(digest));
  return rc;
}
