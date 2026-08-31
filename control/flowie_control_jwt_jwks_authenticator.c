#include "flowie_control_jwt_jwks_authenticator_internal.h"

#include "CoroNet/turbo_coro_context.h"
#include "cjwt/cjwt.h"
#include "http_client.h"
#include "turbo_error.h"
#include "turbo_parser.h"
#include "turbo_str.h"
#include "turbo_thread.h"

#include <openssl/ssl.h>

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct flowie_control_jwt_jwks_snapshot_s {
  cjwt_jwks_t *keys;
  uint64_t valid_until;
} flowie_control_jwt_jwks_snapshot_t;

enum { JWT_JWKS_JOB_OWNER_IDLE = 0, JWT_JWKS_JOB_OWNER_ARMED = 1, JWT_JWKS_JOB_OWNER_DONE = 2 };

typedef struct flowie_control_jwt_jwks_verify_job_s {
  flowie_control_jwt_jwks_authenticator_t *authenticator;
  coro_wait_t *wait;
  atomic_uint references;
  atomic_int completed;
  atomic_int owner_state;
  flowie_control_external_auth_request_t request;
  flowie_control_external_auth_assertion_t assertion;
  uint64_t now;
  int result;
  char domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char presented_identity[FLOWIE_SECURITY_ID_MAX + 1u];
  char method[FLOWIE_SECURITY_TYPE_MAX + 1u];
  uint8_t secret[];
} flowie_control_jwt_jwks_verify_job_t;

typedef struct flowie_control_jwt_jwks_parse_job_s {
  flowie_control_jwt_jwks_authenticator_t *authenticator;
  coro_wait_t *wait;
  atomic_uint references;
  atomic_int completed;
  atomic_int owner_state;
  flowie_control_jwt_jwks_snapshot_t *snapshot;
  uint64_t valid_until;
  size_t json_size;
  int result;
  char json[];
} flowie_control_jwt_jwks_parse_job_t;

struct flowie_control_jwt_jwks_authenticator_s {
  flowie_control_external_authenticator_t interface;
  tstr url;
  tstr host;
  tstr method;
  tstr trusted_issuer;
  tstr audience;
  tstr subject_type;
  tstr algorithm;
  tstr ca_file;
  cjwt_alg_t algorithm_id;
  uint16_t port;
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
  turbo_threadpool_t *executor;
  atomic_int refresh_in_flight;
  turbo_rwlock_t snapshot_lock;
  int snapshot_lock_initialized;
  flowie_control_jwt_jwks_snapshot_t *snapshot;
};

static int jwt_jwks_snapshot_parse(const flowie_control_jwt_jwks_authenticator_t *authenticator,
                                   const char *jwks_json, size_t jwks_size, uint64_t valid_until,
                                   flowie_control_jwt_jwks_snapshot_t **out);
static void jwt_jwks_snapshot_destroy(flowie_control_jwt_jwks_snapshot_t *snapshot);

static void jwt_jwks_verify_job_release(flowie_control_jwt_jwks_verify_job_t *job) {
  size_t allocation_size;
  if (!job || atomic_fetch_sub_explicit(&job->references, 1u, memory_order_acq_rel) != 1u) return;
  allocation_size = sizeof(*job) + job->request.secret_size;
  (void)coro_wait_destroy(job->wait);
  memset(job, 0, allocation_size);
  free(job);
}

static void jwt_jwks_verify_job_run(void *arg) {
  flowie_control_jwt_jwks_verify_job_t *job = (flowie_control_jwt_jwks_verify_job_t *)arg;
  int wake_rc;
  if (!job) return;
  job->result = flowie_control_jwt_jwks_authenticator_verify_token(
      job->authenticator, &job->request, job->now, &job->assertion);
  atomic_store_explicit(&job->completed, 1, memory_order_release);
  while (atomic_load_explicit(&job->owner_state, memory_order_acquire) ==
         JWT_JWKS_JOB_OWNER_ARMED) {
    wake_rc = coro_wait_interrupt(job->wait, TURBO_EINTR);
    if (wake_rc != TURBO_EALREADY) break;
    turbo_thread_yield();
  }
  jwt_jwks_verify_job_release(job);
}

static void jwt_jwks_parse_job_release(flowie_control_jwt_jwks_parse_job_t *job) {
  size_t allocation_size;
  if (!job || atomic_fetch_sub_explicit(&job->references, 1u, memory_order_acq_rel) != 1u) return;
  allocation_size = sizeof(*job) + job->json_size + 1u;
  jwt_jwks_snapshot_destroy(job->snapshot);
  (void)coro_wait_destroy(job->wait);
  memset(job, 0, allocation_size);
  free(job);
}

static void jwt_jwks_parse_job_run(void *arg) {
  flowie_control_jwt_jwks_parse_job_t *job = (flowie_control_jwt_jwks_parse_job_t *)arg;
  int wake_rc;
  if (!job) return;
  job->result = jwt_jwks_snapshot_parse(job->authenticator, job->json, job->json_size,
                                        job->valid_until, &job->snapshot);
  atomic_store_explicit(&job->completed, 1, memory_order_release);
  while (atomic_load_explicit(&job->owner_state, memory_order_acquire) ==
         JWT_JWKS_JOB_OWNER_ARMED) {
    wake_rc = coro_wait_interrupt(job->wait, TURBO_EINTR);
    if (wake_rc != TURBO_EALREADY) break;
    turbo_thread_yield();
  }
  jwt_jwks_parse_job_release(job);
}

static uint64_t jwt_jwks_default_clock(void *ctx) {
  time_t now;
  (void)ctx;
  now = time(NULL);
  return now > 0 ? (uint64_t)now : 0u;
}

static int jwt_jwks_text_valid(const char *value, size_t maximum) {
  size_t length;
  if (!value || maximum == 0u) return 0;
  length = strnlen(value, maximum + 1u);
  if (length == 0u || length > maximum) return 0;
  for (size_t index = 0u; index < length; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (byte < 0x20u || byte == 0x7fu) return 0;
  }
  return 1;
}

static int jwt_jwks_expected_domain_valid(const char *value) {
  return value && (value[0] == '\0' || jwt_jwks_text_valid(value, FLOWIE_SECURITY_ID_MAX));
}

static int jwt_jwks_ascii_equal(const char *left, const char *right) {
  if (!left || !right) return 0;
  while (*left && *right) {
    unsigned char a = (unsigned char)*left++;
    unsigned char b = (unsigned char)*right++;
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
    if (a != b) return 0;
  }
  return *left == '\0' && *right == '\0';
}

static int jwt_jwks_validate_url(const char *url, tstr *host_out, uint16_t *port_out) {
  uri_t *uri = NULL;
  const char *scheme;
  const char *host;
  const char *path;
  int port;
  int rc = TURBO_EINVAL;
  if (!url || !url[0] || !host_out || !port_out ||
      turbo_parse_uri((const uint8_t *)url, strlen(url), &uri) != TURBO_OK || !uri)
    return TURBO_EINVAL;
  scheme = turbo_uri_scheme(uri);
  host = turbo_uri_host(uri);
  path = turbo_uri_path(uri);
  port = turbo_uri_port(uri);
  if (!turbo_uri_is_valid(uri) || !scheme || strcmp(scheme, "https") != 0 || !host || !host[0] ||
      !path || path[0] != '/' || !path[1] ||
      (turbo_uri_userinfo(uri) && turbo_uri_userinfo(uri)[0]) ||
      (turbo_uri_query(uri) && turbo_uri_query(uri)[0]) ||
      (turbo_uri_fragment(uri) && turbo_uri_fragment(uri)[0]) || port < 0 || port > UINT16_MAX)
    goto done;
  *host_out = tstr_dup(host);
  if (!*host_out) {
    rc = TURBO_ENOMEM;
    goto done;
  }
  *port_out = port == 0 ? 443u : (uint16_t)port;
  rc = TURBO_OK;

done:
  turbo_free_uri(&uri);
  return rc;
}

static int jwt_jwks_connect_policy(const char *scheme, const char *hostname, uint16_t port,
                                   const turbo_dns_result_t *results, size_t result_count,
                                   void *user_data) {
  const flowie_control_jwt_jwks_authenticator_t *authenticator =
      (const flowie_control_jwt_jwks_authenticator_t *)user_data;
  (void)results;
  if (!authenticator || !scheme || strcmp(scheme, "https") != 0 || !hostname ||
      !jwt_jwks_ascii_equal(hostname, authenticator->host) || port != authenticator->port ||
      result_count == 0u)
    return -1;
  return 0;
}

static int jwt_jwks_ca_file_validate(const char *ca_file) {
  SSL_CTX *context;
  int rc = TURBO_OK;
  if (!ca_file) return TURBO_OK;
  if (!jwt_jwks_text_valid(ca_file, FLOWIE_CONTROL_JWT_JWKS_URL_MAX)) return TURBO_EINVAL;
  context = SSL_CTX_new(TLS_client_method());
  if (!context) return TURBO_EIO;
  if (SSL_CTX_load_verify_locations(context, ca_file, NULL) != 1) rc = TURBO_EIO;
  SSL_CTX_free(context);
  return rc;
}

static int jwt_jwks_algorithm_valid(cjwt_alg_t algorithm) {
  switch (algorithm) {
  case alg_es256:
  case alg_es384:
  case alg_es512:
  case alg_ps256:
  case alg_ps384:
  case alg_ps512:
  case alg_rs256:
  case alg_rs384:
  case alg_rs512:
  case alg_es256k:
  case alg_eddsa:
    return 1;
  default:
    return 0;
  }
}

static int jwt_jwks_key_type_matches(cjwt_alg_t algorithm, cjwt_kty_t key_type) {
  switch (algorithm) {
  case alg_rs256:
  case alg_rs384:
  case alg_rs512:
  case alg_ps256:
  case alg_ps384:
  case alg_ps512:
    return key_type == CJWT_KTY_RSA;
  case alg_es256:
  case alg_es384:
  case alg_es512:
  case alg_es256k:
    return key_type == CJWT_KTY_EC;
  case alg_eddsa:
    return key_type == CJWT_KTY_OKP;
  default:
    return 0;
  }
}

static int jwt_jwks_json_string_present(const json_value_t *object, const char *key) {
  json_value_t *value;
  if (!object || !key) return 0;
  value = turbo_json_object_get(object, key);
  return value && turbo_json_type(value) == TURBO_JSON_STRING && turbo_json_string(value) &&
         turbo_json_string_len(value) > 0u;
}

static int jwt_jwks_key_has_private_material(const cjwt_jwk_t *key) {
  static const char *const private_fields[] = {"d", "p", "q", "dp", "dq", "qi", "oth", "k"};
  if (!key || !key->key_json) return 1;
  for (size_t index = 0u; index < sizeof(private_fields) / sizeof(private_fields[0]); ++index)
    if (turbo_json_object_get(key->key_json, private_fields[index])) return 1;
  return 0;
}

static int jwt_jwks_public_material_valid(const cjwt_jwk_t *key) {
  if (!key || !key->key_json) return 0;
  if (key->kty == CJWT_KTY_RSA)
    return jwt_jwks_json_string_present(key->key_json, "n") &&
           jwt_jwks_json_string_present(key->key_json, "e");
  if (key->kty == CJWT_KTY_EC)
    return jwt_jwks_json_string_present(key->key_json, "crv") &&
           jwt_jwks_json_string_present(key->key_json, "x") &&
           jwt_jwks_json_string_present(key->key_json, "y");
  if (key->kty == CJWT_KTY_OKP)
    return jwt_jwks_json_string_present(key->key_json, "crv") &&
           jwt_jwks_json_string_present(key->key_json, "x");
  return 0;
}

static int jwt_jwks_snapshot_validate(const flowie_control_jwt_jwks_authenticator_t *authenticator,
                                      const cjwt_jwks_t *keys) {
  if (!authenticator || !keys || keys->count <= 0 ||
      (uint32_t)keys->count > authenticator->max_keys)
    return TURBO_EPROTO;
  for (int index = 0; index < keys->count; ++index) {
    const cjwt_jwk_t *key = keys->keys[index];
    if (!key || !jwt_jwks_text_valid(key->kid, FLOWIE_SECURITY_ID_MAX) || !key->use ||
        strcmp(key->use, "sig") != 0 || !key->alg ||
        strcmp(key->alg, authenticator->algorithm) != 0 ||
        !jwt_jwks_key_type_matches(authenticator->algorithm_id, key->kty) ||
        jwt_jwks_key_has_private_material(key) || !jwt_jwks_public_material_valid(key))
      return TURBO_EPROTO;
    for (int previous = 0; previous < index; ++previous)
      if (strcmp(keys->keys[previous]->kid, key->kid) == 0) return TURBO_EPROTO;
  }
  return TURBO_OK;
}

static void jwt_jwks_snapshot_destroy(flowie_control_jwt_jwks_snapshot_t *snapshot) {
  if (!snapshot) return;
  cjwt_jwks_destroy(snapshot->keys);
  memset(snapshot, 0, sizeof(*snapshot));
  free(snapshot);
}

static int jwt_jwks_audience_matches(const cjwt_t *token, const char *expected) {
  if (!token || !expected || token->aud.count <= 0 || !token->aud.names) return 0;
  for (int index = 0; index < token->aud.count; ++index)
    if (token->aud.names[index] && strcmp(token->aud.names[index], expected) == 0) return 1;
  return 0;
}

static int jwt_jwks_claim_occurrences(const json_value_t *claims, const char *name) {
  int count = 0;
  if (!claims || turbo_json_type(claims) != TURBO_JSON_OBJECT || !name) return 0;
  for (size_t index = 0u; index < turbo_json_object_size(claims); ++index) {
    const char *key = turbo_json_object_key(claims, index);
    if (key && strcmp(key, name) == 0) ++count;
  }
  return count;
}

static int jwt_jwks_claim_text(const json_value_t *claims, const char *name, char *destination,
                               size_t capacity) {
  json_value_t *value;
  const char *text;
  size_t length;
  if (!claims || !name || !destination || capacity == 0u ||
      jwt_jwks_claim_occurrences(claims, name) != 1)
    return TURBO_EPROTO;
  value = turbo_json_object_get(claims, name);
  if (!value || turbo_json_type(value) != TURBO_JSON_STRING) return TURBO_EPROTO;
  text = turbo_json_string(value);
  length = turbo_json_string_len(value);
  if (!text || length == 0u || length >= capacity || memchr(text, '\0', length))
    return TURBO_EPROTO;
  memcpy(destination, text, length);
  destination[length] = '\0';
  return jwt_jwks_text_valid(destination, capacity - 1u) ? TURBO_OK : TURBO_EPROTO;
}

static int jwt_jwks_claim_u64(const json_value_t *claims, const char *name, uint64_t *out) {
  json_value_t *value;
  double number;
  uint64_t converted;
  if (out) *out = 0u;
  if (!claims || !name || !out || jwt_jwks_claim_occurrences(claims, name) != 1)
    return TURBO_EPROTO;
  value = turbo_json_object_get(claims, name);
  if (!value || turbo_json_type(value) != TURBO_JSON_NUMBER) return TURBO_EPROTO;
  number = turbo_json_number(value);
  if (!isfinite(number) || number < 1.0 || number > 9007199254740991.0) return TURBO_EPROTO;
  converted = (uint64_t)number;
  if ((double)converted != number) return TURBO_EPROTO;
  *out = converted;
  return TURBO_OK;
}

static int jwt_jwks_claim_enabled(const json_value_t *claims) {
  json_value_t *value;
  if (!claims || jwt_jwks_claim_occurrences(claims, "account_enabled") != 1) return TURBO_EPROTO;
  value = turbo_json_object_get(claims, "account_enabled");
  if (!value || turbo_json_type(value) != TURBO_JSON_BOOL) return TURBO_EPROTO;
  return turbo_json_bool(value) ? TURBO_OK : TURBO_EPERM;
}

static int jwt_jwks_copy_groups(const json_value_t *claims,
                                flowie_control_external_auth_assertion_t *assertion) {
  json_value_t *groups;
  size_t count;
  if (!claims || !assertion) return TURBO_EPROTO;
  if (jwt_jwks_claim_occurrences(claims, "groups") == 0) return TURBO_OK;
  if (jwt_jwks_claim_occurrences(claims, "groups") != 1) return TURBO_EPROTO;
  groups = turbo_json_object_get(claims, "groups");
  if (!groups || turbo_json_type(groups) != TURBO_JSON_ARRAY) return TURBO_EPROTO;
  count = turbo_json_array_size(groups);
  if (count > FLOWIE_SECURITY_MAX_GROUPS) return TURBO_EPROTO;
  for (size_t index = 0u; index < count; ++index) {
    json_value_t *entry = turbo_json_array_get(groups, index);
    const char *text;
    size_t length;
    if (!entry || turbo_json_type(entry) != TURBO_JSON_STRING) return TURBO_EPROTO;
    text = turbo_json_string(entry);
    length = turbo_json_string_len(entry);
    if (!text || length == 0u || length > FLOWIE_SECURITY_ID_MAX || memchr(text, '\0', length))
      return TURBO_EPROTO;
    memcpy(assertion->external_groups[index], text, length);
    assertion->external_groups[index][length] = '\0';
    if (!jwt_jwks_text_valid(assertion->external_groups[index], FLOWIE_SECURITY_ID_MAX))
      return TURBO_EPROTO;
    for (size_t previous = 0u; previous < index; ++previous)
      if (strcmp(assertion->external_groups[previous], assertion->external_groups[index]) == 0)
        return TURBO_EPROTO;
  }
  assertion->external_group_count = (uint32_t)count;
  return TURBO_OK;
}

static int
jwt_jwks_assertion_from_token(const flowie_control_jwt_jwks_authenticator_t *authenticator,
                              const flowie_control_external_auth_request_t *request,
                              const cjwt_t *token, uint64_t now,
                              flowie_control_external_auth_assertion_t *assertion_out) {
  flowie_control_external_auth_assertion_t assertion = FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  uint64_t assurance = 0u;
  int rc;
  if (!authenticator || !request || !token || !assertion_out || !token->iss || !token->sub ||
      !token->exp || !token->nbf || !token->iat || *token->exp <= 0 || *token->nbf <= 0 ||
      *token->iat <= 0 || strcmp(token->iss, authenticator->trusted_issuer) != 0 ||
      strcmp(token->sub, request->presented_identity) != 0 ||
      !jwt_jwks_audience_matches(token, authenticator->audience))
    return TURBO_EPERM;
  if ((uint64_t)*token->iat > now) return TURBO_EPERM;
  rc = jwt_jwks_claim_text(token->private_claims, "domain_id", assertion.domain_id,
                           sizeof(assertion.domain_id));
  if (rc != TURBO_OK) return rc;
  if (request->domain_id[0] && strcmp(assertion.domain_id, request->domain_id) != 0)
    return TURBO_EPERM;
  rc = jwt_jwks_claim_enabled(token->private_claims);
  if (rc != TURBO_OK) return rc;
  rc = jwt_jwks_claim_u64(token->private_claims, "revision", &assertion.revision);
  if (rc != TURBO_OK) return rc;
  rc = jwt_jwks_claim_u64(token->private_claims, "assurance_level", &assurance);
  if (rc != TURBO_OK || assurance < FLOWIE_CONTROL_EXTERNAL_ASSURANCE_SINGLE_FACTOR ||
      assurance > FLOWIE_CONTROL_EXTERNAL_ASSURANCE_HARDWARE_BOUND)
    return TURBO_EPROTO;
  rc = jwt_jwks_copy_groups(token->private_claims, &assertion);
  if (rc != TURBO_OK) return rc;
  memcpy(assertion.issuer, authenticator->trusted_issuer,
         strlen(authenticator->trusted_issuer) + 1u);
  memcpy(assertion.subject, token->sub, strlen(token->sub) + 1u);
  memcpy(assertion.subject_type, authenticator->subject_type,
         strlen(authenticator->subject_type) + 1u);
  memcpy(assertion.auth_method, authenticator->method, strlen(authenticator->method) + 1u);
  assertion.issued_at = (uint64_t)*token->iat;
  assertion.expires_at = (uint64_t)*token->exp;
  assertion.assurance_level = (uint32_t)assurance;
  assertion.account_enabled = 1;
  *assertion_out = assertion;
  return TURBO_OK;
}

static int jwt_jwks_snapshot_parse(const flowie_control_jwt_jwks_authenticator_t *authenticator,
                                   const char *jwks_json, size_t jwks_size, uint64_t valid_until,
                                   flowie_control_jwt_jwks_snapshot_t **out) {
  flowie_control_jwt_jwks_snapshot_t *next = NULL;
  char *document = NULL;
  int rc = TURBO_EPROTO;
  if (out) *out = NULL;
  if (!authenticator || !jwks_json || jwks_size == 0u || !out ||
      jwks_size > authenticator->max_response_size || valid_until == 0u ||
      memchr(jwks_json, '\0', jwks_size))
    return TURBO_EINVAL;
  document = (char *)malloc(jwks_size + 1u);
  next = (flowie_control_jwt_jwks_snapshot_t *)calloc(1u, sizeof(*next));
  if (!document || !next) {
    rc = TURBO_ENOMEM;
    goto done;
  }
  memcpy(document, jwks_json, jwks_size);
  document[jwks_size] = '\0';
  if (cjwt_jwks_parse(document, &next->keys) != CJWTE_OK || !next->keys) goto done;
  rc = jwt_jwks_snapshot_validate(authenticator, next->keys);
  if (rc != TURBO_OK) goto done;
  next->valid_until = valid_until;
  *out = next;
  next = NULL;
  rc = TURBO_OK;

done:
  free(document);
  jwt_jwks_snapshot_destroy(next);
  return rc;
}

static void jwt_jwks_snapshot_replace(flowie_control_jwt_jwks_authenticator_t *authenticator,
                                      flowie_control_jwt_jwks_snapshot_t *next) {
  flowie_control_jwt_jwks_snapshot_t *previous;
  turbo_rwlock_wrlock(&authenticator->snapshot_lock);
  previous = authenticator->snapshot;
  authenticator->snapshot = next;
  turbo_rwlock_wrunlock(&authenticator->snapshot_lock);
  jwt_jwks_snapshot_destroy(previous);
}

int flowie_control_jwt_jwks_authenticator_install(
    flowie_control_jwt_jwks_authenticator_t *authenticator, const char *jwks_json, size_t jwks_size,
    uint64_t valid_until) {
  flowie_control_jwt_jwks_snapshot_t *next = NULL;
  int rc = jwt_jwks_snapshot_parse(authenticator, jwks_json, jwks_size, valid_until, &next);
  if (rc != TURBO_OK) return rc;
  jwt_jwks_snapshot_replace(authenticator, next);
  return TURBO_OK;
}

int flowie_control_jwt_jwks_authenticator_verify_token(
    flowie_control_jwt_jwks_authenticator_t *authenticator,
    const flowie_control_external_auth_request_t *request, uint64_t now,
    flowie_control_external_auth_assertion_t *assertion_out) {
  flowie_control_external_auth_assertion_t empty = FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  int rc = TURBO_EPERM;
  if (assertion_out && assertion_out->size >= sizeof(*assertion_out)) *assertion_out = empty;
  if (!authenticator || !request || request->size < sizeof(*request) || !assertion_out ||
      assertion_out->size < sizeof(*assertion_out) || now == 0u ||
      now > (uint64_t)INT64_MAX - (uint64_t)authenticator->clock_skew_seconds ||
      !jwt_jwks_expected_domain_valid(request->domain_id) ||
      !jwt_jwks_text_valid(request->presented_identity, FLOWIE_SECURITY_ID_MAX) ||
      !request->method || strcmp(request->method, authenticator->method) != 0 || !request->secret ||
      request->secret_size == 0u || request->secret_size > authenticator->max_token_size ||
      memchr(request->secret, '\0', request->secret_size))
    return TURBO_EINVAL;
  turbo_rwlock_rdlock(&authenticator->snapshot_lock);
  if (!authenticator->snapshot || authenticator->snapshot->valid_until <= now) {
    turbo_rwlock_rdunlock(&authenticator->snapshot_lock);
    return TURBO_EBUSY;
  }
  for (int index = 0; index < authenticator->snapshot->keys->count; ++index) {
    const cjwt_jwk_t *key = authenticator->snapshot->keys->keys[index];
    cjwt_t *token = NULL;
    cjwt_code_t decoded =
        cjwt_decode_with_jwk((const char *)request->secret, request->secret_size, 0u, key,
                             (int64_t)now, (int64_t)authenticator->clock_skew_seconds, &token);
    if (decoded == CJWTE_OUT_OF_MEMORY) {
      rc = TURBO_ENOMEM;
      break;
    }
    if (decoded != CJWTE_OK || !token) continue;
    if (!token->header.kid || strcmp(token->header.kid, key->kid) != 0 ||
        token->header.alg != authenticator->algorithm_id) {
      cjwt_destroy(token);
      rc = TURBO_EPERM;
      break;
    }
    rc = jwt_jwks_assertion_from_token(authenticator, request, token, now, assertion_out);
    cjwt_destroy(token);
    break;
  }
  turbo_rwlock_rdunlock(&authenticator->snapshot_lock);
  if (rc != TURBO_OK) *assertion_out = empty;
  return rc;
}

static int jwt_jwks_content_type_json(const char *headers, size_t headers_size) {
  static const char name[] = "content-type:";
  static const char media_type[] = "application/json";
  size_t line_start = 0u;
  int matches = 0;
  if (!headers || headers_size == 0u) return 0;
  while (line_start < headers_size) {
    size_t line_end = line_start;
    size_t cursor;
    while (line_end < headers_size && headers[line_end] != '\n')
      ++line_end;
    cursor = line_start;
    if (line_end - cursor >= sizeof(name) - 1u) {
      size_t index;
      for (index = 0u; index < sizeof(name) - 1u; ++index) {
        unsigned char byte = (unsigned char)headers[cursor + index];
        if (byte >= 'A' && byte <= 'Z') byte = (unsigned char)(byte + ('a' - 'A'));
        if (byte != (unsigned char)name[index]) break;
      }
      if (index == sizeof(name) - 1u) {
        cursor += sizeof(name) - 1u;
        while (cursor < line_end && (headers[cursor] == ' ' || headers[cursor] == '\t'))
          ++cursor;
        if (line_end - cursor < sizeof(media_type) - 1u) return 0;
        for (index = 0u; index < sizeof(media_type) - 1u; ++index) {
          unsigned char byte = (unsigned char)headers[cursor + index];
          if (byte >= 'A' && byte <= 'Z') byte = (unsigned char)(byte + ('a' - 'A'));
          if (byte != (unsigned char)media_type[index]) return 0;
        }
        cursor += sizeof(media_type) - 1u;
        if (cursor < line_end && headers[cursor] != ';' && headers[cursor] != '\r') return 0;
        ++matches;
      }
    }
    line_start = line_end + 1u;
  }
  return matches == 1;
}

static int jwt_jwks_parse_offloaded(flowie_control_jwt_jwks_authenticator_t *authenticator,
                                    const char *json, size_t json_size, uint64_t valid_until,
                                    flowie_control_jwt_jwks_snapshot_t **snapshot_out) {
  flowie_control_jwt_jwks_parse_job_t *job;
  coro_context_t *context;
  int completed;
  int wait_rc = TURBO_OK;
  int rc;
  if (snapshot_out) *snapshot_out = NULL;
  if (!authenticator || !json || json_size == 0u || !snapshot_out ||
      json_size > SIZE_MAX - sizeof(*job) - 1u)
    return TURBO_EINVAL;
  context = coro_context_current();
  if (!context) return TURBO_ENOTSUP;
  job = (flowie_control_jwt_jwks_parse_job_t *)calloc(1u, sizeof(*job) + json_size + 1u);
  if (!job) return TURBO_ENOMEM;
  job->wait = coro_wait_create(context);
  if (!job->wait) {
    free(job);
    return TURBO_ENOMEM;
  }
  job->authenticator = authenticator;
  job->valid_until = valid_until;
  job->json_size = json_size;
  job->result = TURBO_EIO;
  memcpy(job->json, json, json_size);
  job->json[json_size] = '\0';
  atomic_init(&job->references, 2u);
  atomic_init(&job->completed, 0);
  atomic_init(&job->owner_state, JWT_JWKS_JOB_OWNER_ARMED);
  if (turbo_threadpool_try_submit(authenticator->executor, jwt_jwks_parse_job_run, job) !=
      TURBO_OK) {
    atomic_store_explicit(&job->owner_state, JWT_JWKS_JOB_OWNER_DONE, memory_order_release);
    jwt_jwks_parse_job_release(job);
    jwt_jwks_parse_job_release(job);
    return TURBO_EBUSY;
  }
  completed = atomic_load_explicit(&job->completed, memory_order_acquire);
  if (!completed) wait_rc = coro_wait_for(job->wait, authenticator->executor_deadline_ms);
  atomic_store_explicit(&job->owner_state, JWT_JWKS_JOB_OWNER_DONE, memory_order_release);
  completed = atomic_load_explicit(&job->completed, memory_order_acquire);
  if (completed) {
    rc = job->result;
    if (rc == TURBO_OK) {
      *snapshot_out = job->snapshot;
      job->snapshot = NULL;
    }
  } else {
    rc = wait_rc == TURBO_OK ? TURBO_ETIMEDOUT : wait_rc;
  }
  jwt_jwks_parse_job_release(job);
  return rc;
}

static int jwt_jwks_fetch_snapshot(flowie_control_jwt_jwks_authenticator_t *authenticator,
                                   uint64_t now) {
  http_client_t *client = NULL;
  http_response_t *response = NULL;
  flowie_control_jwt_jwks_snapshot_t *snapshot = NULL;
  turbo_tls_client_config_t tls = {0};
  const char *headers[] = {"Accept: application/json"};
  uint64_t valid_until;
  int rc = TURBO_EIO;
  if (!authenticator || now == 0u || now > UINT64_MAX - authenticator->refresh_interval_seconds)
    return TURBO_EINVAL;
  valid_until = now + authenticator->refresh_interval_seconds;
  client = http_client_create(authenticator->url);
  if (!client) goto done;
  http_client_set_timeout(client, (int)authenticator->timeout_ms);
  http_client_set_connect_timeout(client, (int)authenticator->timeout_ms);
  http_client_set_read_timeout(client, (int)authenticator->timeout_ms);
  http_client_set_max_response_size(client, authenticator->max_response_size);
  http_client_set_max_response_header_size(client, 16384u);
  http_client_follow_redirects(client, 0);
  http_client_clear_retry_policy(client);
  tls.ca_file = authenticator->ca_file;
  tls.verify_peer = 1;
  if (http_client_set_tls_client_config(client, &tls) != 0) goto done;
  if (http_client_set_connect_policy(client, jwt_jwks_connect_policy, authenticator) != 0)
    goto done;
  response = http_request(client, HTTP_GET, authenticator->url, headers, 1u, NULL, 0u);
  if (!response || response->error_code != HTTP_ERROR_NONE || response->status_code <= 0) goto done;
  if (response->status_code == 429) {
    rc = TURBO_EBUSY;
    goto done;
  }
  if (response->status_code != 200 || !response->body || response->body_len == 0u ||
      response->body_len > authenticator->max_response_size ||
      !jwt_jwks_content_type_json(response->headers, response->headers_len)) {
    rc = TURBO_EPROTO;
    goto done;
  }
  rc = jwt_jwks_parse_offloaded(authenticator, response->body, response->body_len, valid_until,
                                &snapshot);
  if (rc != TURBO_OK) goto done;
  jwt_jwks_snapshot_replace(authenticator, snapshot);
  snapshot = NULL;

done:
  jwt_jwks_snapshot_destroy(snapshot);
  if (response) http_response_free(response);
  if (client) http_client_destroy(client);
  return rc;
}

static int jwt_jwks_refresh_if_needed(flowie_control_jwt_jwks_authenticator_t *authenticator,
                                      uint64_t now) {
  int expected = 0;
  int current = 0;
  int rc;
  if (!authenticator || now == 0u) return TURBO_EINVAL;
  turbo_rwlock_rdlock(&authenticator->snapshot_lock);
  if (authenticator->snapshot && authenticator->snapshot->valid_until > now) current = 1;
  turbo_rwlock_rdunlock(&authenticator->snapshot_lock);
  if (current) return TURBO_OK;
  if (!atomic_compare_exchange_strong_explicit(&authenticator->refresh_in_flight, &expected, 1,
                                               memory_order_acq_rel, memory_order_acquire))
    return TURBO_EBUSY;
  turbo_rwlock_rdlock(&authenticator->snapshot_lock);
  if (authenticator->snapshot && authenticator->snapshot->valid_until > now) current = 1;
  turbo_rwlock_rdunlock(&authenticator->snapshot_lock);
  rc = current ? TURBO_OK : jwt_jwks_fetch_snapshot(authenticator, now);
  atomic_store_explicit(&authenticator->refresh_in_flight, 0, memory_order_release);
  return rc;
}

static int jwt_jwks_verify(void *ctx, const flowie_control_external_auth_request_t *request,
                           flowie_control_external_auth_assertion_t *assertion_out) {
  flowie_control_jwt_jwks_authenticator_t *authenticator =
      (flowie_control_jwt_jwks_authenticator_t *)ctx;
  flowie_control_jwt_jwks_verify_job_t *job;
  coro_context_t *context;
  uint64_t now;
  int completed;
  int wait_rc = TURBO_OK;
  int rc;
  if (assertion_out && assertion_out->size >= sizeof(*assertion_out))
    *assertion_out =
        (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  if (!authenticator || !request || request->size < sizeof(*request) || !assertion_out ||
      assertion_out->size < sizeof(*assertion_out) ||
      !jwt_jwks_expected_domain_valid(request->domain_id) ||
      !jwt_jwks_text_valid(request->presented_identity, FLOWIE_SECURITY_ID_MAX) ||
      !jwt_jwks_text_valid(request->method, FLOWIE_SECURITY_TYPE_MAX) || !request->secret ||
      request->secret_size == 0u || request->secret_size > authenticator->max_token_size)
    return TURBO_EINVAL;
  context = coro_context_current();
  if (!context) return TURBO_ENOTSUP;
  now = authenticator->clock_seconds(authenticator->clock_ctx);
  if (now == 0u || now > (uint64_t)INT64_MAX - (uint64_t)authenticator->clock_skew_seconds)
    return TURBO_EIO;
  rc = jwt_jwks_refresh_if_needed(authenticator, now);
  if (rc != TURBO_OK) return rc;
  if (request->secret_size > SIZE_MAX - sizeof(*job)) return TURBO_ERANGE;
  job = (flowie_control_jwt_jwks_verify_job_t *)calloc(1u, sizeof(*job) + request->secret_size);
  if (!job) return TURBO_ENOMEM;
  job->wait = coro_wait_create(context);
  if (!job->wait) {
    free(job);
    return TURBO_ENOMEM;
  }
  job->authenticator = authenticator;
  job->request = (flowie_control_external_auth_request_t)FLOWIE_CONTROL_EXTERNAL_AUTH_REQUEST_INIT;
  memcpy(job->domain_id, request->domain_id, strlen(request->domain_id) + 1u);
  memcpy(job->presented_identity, request->presented_identity,
         strlen(request->presented_identity) + 1u);
  memcpy(job->method, request->method, strlen(request->method) + 1u);
  memcpy(job->secret, request->secret, request->secret_size);
  job->request.domain_id = job->domain_id;
  job->request.presented_identity = job->presented_identity;
  job->request.method = job->method;
  job->request.secret = job->secret;
  job->request.secret_size = request->secret_size;
  job->now = now;
  job->assertion =
      (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  job->result = TURBO_EIO;
  atomic_init(&job->references, 2u);
  atomic_init(&job->completed, 0);
  atomic_init(&job->owner_state, JWT_JWKS_JOB_OWNER_ARMED);
  if (turbo_threadpool_try_submit(authenticator->executor, jwt_jwks_verify_job_run, job) !=
      TURBO_OK) {
    atomic_store_explicit(&job->owner_state, JWT_JWKS_JOB_OWNER_DONE, memory_order_release);
    jwt_jwks_verify_job_release(job);
    jwt_jwks_verify_job_release(job);
    return TURBO_EBUSY;
  }
  completed = atomic_load_explicit(&job->completed, memory_order_acquire);
  if (!completed) wait_rc = coro_wait_for(job->wait, authenticator->executor_deadline_ms);
  atomic_store_explicit(&job->owner_state, JWT_JWKS_JOB_OWNER_DONE, memory_order_release);
  completed = atomic_load_explicit(&job->completed, memory_order_acquire);
  if (completed) {
    rc = job->result;
    if (rc == TURBO_OK) *assertion_out = job->assertion;
  } else {
    rc = wait_rc == TURBO_OK ? TURBO_ETIMEDOUT : wait_rc;
  }
  jwt_jwks_verify_job_release(job);
  return rc;
}

int flowie_control_jwt_jwks_authenticator_create(
    const flowie_control_jwt_jwks_authenticator_config_t *config,
    flowie_control_jwt_jwks_authenticator_t **out) {
  flowie_control_jwt_jwks_authenticator_t *authenticator = NULL;
  cjwt_alg_t algorithm = alg_none;
  int rc = TURBO_ENOMEM;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) || !out ||
      !jwt_jwks_text_valid(config->url, FLOWIE_CONTROL_JWT_JWKS_URL_MAX) ||
      strncmp(config->url, "https://", sizeof("https://") - 1u) != 0 ||
      !jwt_jwks_text_valid(config->method, FLOWIE_SECURITY_TYPE_MAX) ||
      !jwt_jwks_text_valid(config->trusted_issuer, FLOWIE_CONTROL_EXTERNAL_ISSUER_MAX) ||
      !jwt_jwks_text_valid(config->audience, FLOWIE_SECURITY_ID_MAX) ||
      !jwt_jwks_text_valid(config->subject_type, FLOWIE_SECURITY_TYPE_MAX) ||
      !jwt_jwks_text_valid(config->algorithm, FLOWIE_CONTROL_JWT_JWKS_ALGORITHM_MAX) ||
      (config->ca_file && !jwt_jwks_text_valid(config->ca_file, FLOWIE_CONTROL_JWT_JWKS_URL_MAX)) ||
      cjwt_alg_string_to_enum(config->algorithm, SIZE_MAX, &algorithm) != CJWTE_OK ||
      !jwt_jwks_algorithm_valid(algorithm) || config->timeout_ms == 0u ||
      config->timeout_ms > FLOWIE_CONTROL_JWT_JWKS_MAX_TIMEOUT_MS ||
      config->max_response_size == 0u ||
      config->max_response_size > FLOWIE_CONTROL_JWT_JWKS_MAX_RESPONSE_SIZE ||
      config->max_keys == 0u || config->max_keys > FLOWIE_CONTROL_JWT_JWKS_MAX_KEYS ||
      config->max_token_size == 0u ||
      config->max_token_size > FLOWIE_CONTROL_JWT_JWKS_MAX_TOKEN_SIZE ||
      config->refresh_interval_seconds == 0u ||
      config->refresh_interval_seconds > FLOWIE_CONTROL_JWT_JWKS_MAX_REFRESH_SECONDS ||
      config->clock_skew_seconds > FLOWIE_CONTROL_JWT_JWKS_MAX_CLOCK_SKEW_SECONDS ||
      config->executor_workers == 0u ||
      config->executor_workers > FLOWIE_CONTROL_JWT_JWKS_MAX_WORKERS ||
      config->executor_queue_capacity == 0u ||
      config->executor_queue_capacity > FLOWIE_CONTROL_JWT_JWKS_MAX_QUEUE_CAPACITY ||
      config->executor_deadline_ms == 0u ||
      config->executor_deadline_ms > FLOWIE_CONTROL_JWT_JWKS_MAX_DEADLINE_MS)
    return TURBO_EINVAL;
  authenticator = (flowie_control_jwt_jwks_authenticator_t *)calloc(1u, sizeof(*authenticator));
  if (!authenticator) return TURBO_ENOMEM;
  authenticator->url = tstr_dup(config->url);
  authenticator->method = tstr_dup(config->method);
  authenticator->trusted_issuer = tstr_dup(config->trusted_issuer);
  authenticator->audience = tstr_dup(config->audience);
  authenticator->subject_type = tstr_dup(config->subject_type);
  authenticator->algorithm = tstr_dup(config->algorithm);
  authenticator->ca_file = config->ca_file ? tstr_dup(config->ca_file) : NULL;
  if (!authenticator->url || !authenticator->method || !authenticator->trusted_issuer ||
      !authenticator->audience || !authenticator->subject_type || !authenticator->algorithm ||
      (config->ca_file && !authenticator->ca_file))
    goto fail;
  rc = jwt_jwks_validate_url(authenticator->url, &authenticator->host, &authenticator->port);
  if (rc != TURBO_OK) goto fail;
  rc = jwt_jwks_ca_file_validate(authenticator->ca_file);
  if (rc != TURBO_OK) goto fail;
  authenticator->algorithm_id = algorithm;
  authenticator->timeout_ms = config->timeout_ms;
  authenticator->max_response_size = config->max_response_size;
  authenticator->max_keys = config->max_keys;
  authenticator->max_token_size = config->max_token_size;
  authenticator->refresh_interval_seconds = config->refresh_interval_seconds;
  authenticator->clock_skew_seconds = config->clock_skew_seconds;
  authenticator->executor_workers = config->executor_workers;
  authenticator->executor_queue_capacity = config->executor_queue_capacity;
  authenticator->executor_deadline_ms = config->executor_deadline_ms;
  authenticator->clock_seconds =
      config->clock_seconds ? config->clock_seconds : jwt_jwks_default_clock;
  authenticator->clock_ctx = config->clock_ctx;
  atomic_init(&authenticator->refresh_in_flight, 0);
  if (turbo_rwlock_init(&authenticator->snapshot_lock) != TURBO_OK) goto fail;
  authenticator->snapshot_lock_initialized = 1;
  {
    turbo_threadpool_config_t executor_config = {(int)config->executor_workers,
                                                 config->executor_queue_capacity};
    authenticator->executor = turbo_threadpool_create_with_config(&executor_config);
  }
  if (!authenticator->executor) goto fail;
  authenticator->interface =
      (flowie_control_external_authenticator_t)FLOWIE_CONTROL_EXTERNAL_AUTHENTICATOR_INIT;
  authenticator->interface.capabilities = FLOWIE_CONTROL_EXTERNAL_AUTH_REQUIRED_CAPABILITIES |
                                          FLOWIE_CONTROL_EXTERNAL_AUTH_GROUP_CLAIMS;
  authenticator->interface.ctx = authenticator;
  authenticator->interface.method = authenticator->method;
  authenticator->interface.verify = jwt_jwks_verify;
  *out = authenticator;
  return TURBO_OK;

fail:
  flowie_control_jwt_jwks_authenticator_destroy(authenticator);
  return rc;
}

void flowie_control_jwt_jwks_authenticator_destroy(
    flowie_control_jwt_jwks_authenticator_t *authenticator) {
  if (!authenticator) return;
  turbo_threadpool_destroy(authenticator->executor);
  authenticator->executor = NULL;
  jwt_jwks_snapshot_destroy(authenticator->snapshot);
  authenticator->snapshot = NULL;
  if (authenticator->snapshot_lock_initialized) turbo_rwlock_destroy(&authenticator->snapshot_lock);
  tstr_freep(&authenticator->url);
  tstr_freep(&authenticator->host);
  tstr_freep(&authenticator->method);
  tstr_freep(&authenticator->trusted_issuer);
  tstr_freep(&authenticator->audience);
  tstr_freep(&authenticator->subject_type);
  tstr_freep(&authenticator->algorithm);
  tstr_freep(&authenticator->ca_file);
  memset(authenticator, 0, sizeof(*authenticator));
  free(authenticator);
}

const flowie_control_external_authenticator_t *flowie_control_jwt_jwks_authenticator_interface(
    const flowie_control_jwt_jwks_authenticator_t *authenticator) {
  return authenticator ? &authenticator->interface : NULL;
}
