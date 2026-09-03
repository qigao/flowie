#include "flowie_control_jwt_jwks_authenticator_internal.h"

#include <chttp/chttp.h>
#include "cjwt/cjwt.h"
#include "platform.h"
#include "salts_error.h"
#include <json_parser.h>
#include <uri_parser.h>
#include "salts_str.h"
#include "salts_thread.h"

#include <openssl/ssl.h>

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct flowie_control_jwt_jwks_snapshot_s {
  cjwt_jwks_t *keys;
  uint64_t valid_until;
} flowie_control_jwt_jwks_snapshot_t;

enum {
  JWT_JWKS_HEADER_LIMIT = 16384u,
  JWT_JWKS_DESTROY_TIMEOUT_MS = 5000u
};

#define JWT_JWKS_NANOSECONDS_PER_MILLISECOND UINT64_C(1000000)

typedef struct flowie_control_jwt_jwks_verify_job_s {
  flowie_control_jwt_jwks_authenticator_t *authenticator;
  salts_mutex_t mutex;
  salts_cond_t completed_changed;
  atomic_uint references;
  int completed;
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
  salts_mutex_t mutex;
  salts_cond_t completed_changed;
  atomic_uint references;
  int completed;
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
  salts_threadpool_t *executor;
  atomic_int refresh_in_flight;
  salts_rwlock_t snapshot_lock;
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
  salts_cond_destroy(&job->completed_changed);
  salts_mutex_destroy(&job->mutex);
  memset(job, 0, allocation_size);
  free(job);
}

static void jwt_jwks_verify_job_run(void *arg) {
  flowie_control_jwt_jwks_verify_job_t *job = (flowie_control_jwt_jwks_verify_job_t *)arg;
  if (!job) return;
  job->result = flowie_control_jwt_jwks_authenticator_verify_token(
      job->authenticator, &job->request, job->now, &job->assertion);
  salts_mutex_lock(&job->mutex);
  job->completed = 1;
  salts_cond_signal(&job->completed_changed);
  salts_mutex_unlock(&job->mutex);
  jwt_jwks_verify_job_release(job);
}

static void jwt_jwks_parse_job_release(flowie_control_jwt_jwks_parse_job_t *job) {
  size_t allocation_size;
  if (!job || atomic_fetch_sub_explicit(&job->references, 1u, memory_order_acq_rel) != 1u) return;
  allocation_size = sizeof(*job) + job->json_size + 1u;
  jwt_jwks_snapshot_destroy(job->snapshot);
  salts_cond_destroy(&job->completed_changed);
  salts_mutex_destroy(&job->mutex);
  memset(job, 0, allocation_size);
  free(job);
}

static void jwt_jwks_parse_job_run(void *arg) {
  flowie_control_jwt_jwks_parse_job_t *job = (flowie_control_jwt_jwks_parse_job_t *)arg;
  if (!job) return;
  job->result = jwt_jwks_snapshot_parse(job->authenticator, job->json, job->json_size,
                                        job->valid_until, &job->snapshot);
  salts_mutex_lock(&job->mutex);
  job->completed = 1;
  salts_cond_signal(&job->completed_changed);
  salts_mutex_unlock(&job->mutex);
  jwt_jwks_parse_job_release(job);
}

static uint64_t jwt_jwks_wait_deadline(uint32_t timeout_ms) {
  uint64_t now = salts_hrtime();
  if ((uint64_t)timeout_ms >
      (UINT64_MAX - now) / JWT_JWKS_NANOSECONDS_PER_MILLISECOND)
    return UINT64_MAX;
  return now + (uint64_t)timeout_ms * JWT_JWKS_NANOSECONDS_PER_MILLISECOND;
}

static int jwt_jwks_wait(salts_cond_t *changed, salts_mutex_t *mutex, int *completed,
                         uint32_t timeout_ms) {
  uint64_t deadline;
  if (!changed || !mutex || !completed || timeout_ms == 0u) return SALTS_EINVAL;
  deadline = jwt_jwks_wait_deadline(timeout_ms);
  while (!*completed) {
    uint64_t now = salts_hrtime();
    if (now >= deadline ||
        salts_cond_timedwait(changed, mutex, deadline - now) != SALTS_OK)
      return SALTS_ETIMEDOUT;
  }
  return SALTS_OK;
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

static int jwt_jwks_validate_url(const char *url, tstr *host_out, uint16_t *port_out) {
  uri_t uri = {0};
  const char *scheme;
  const char *host;
  const char *path;
  int port;
  int rc = SALTS_EINVAL;
  if (!url || !url[0] || !host_out || !port_out || !uri_parse(url, &uri))
    return SALTS_EINVAL;
  scheme = uri.scheme;
  host = uri.host;
  path = uri.path;
  port = uri.port;
  if (!uri.valid || strcmp(scheme, "https") != 0 || !host[0] || path[0] != '/' || !path[1] ||
      uri.userinfo[0] || uri.query[0] || uri.fragment[0] || port < 0 || port > UINT16_MAX)
    goto done;
  *host_out = tstr_dup(host);
  if (!*host_out) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  *port_out = port == 0 ? 443u : (uint16_t)port;
  rc = SALTS_OK;

done:
  return rc;
}

static int jwt_jwks_ca_file_validate(const char *ca_file) {
  SSL_CTX *context;
  int rc = SALTS_OK;
  if (!ca_file) return SALTS_OK;
  if (!jwt_jwks_text_valid(ca_file, FLOWIE_CONTROL_JWT_JWKS_URL_MAX)) return SALTS_EINVAL;
  context = SSL_CTX_new(TLS_client_method());
  if (!context) return SALTS_EIO;
  if (SSL_CTX_load_verify_locations(context, ca_file, NULL) != 1) rc = SALTS_EIO;
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
  value = json_object_get(object, key);
  return value && json_type(value) == JSON_STRING && json_string(value) &&
         json_string_len(value) > 0u;
}

static int jwt_jwks_key_has_private_material(const cjwt_jwk_t *key) {
  static const char *const private_fields[] = {"d", "p", "q", "dp", "dq", "qi", "oth", "k"};
  if (!key || !key->key_json) return 1;
  for (size_t index = 0u; index < sizeof(private_fields) / sizeof(private_fields[0]); ++index)
    if (json_object_get(key->key_json, private_fields[index])) return 1;
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
    return SALTS_EPROTO;
  for (int index = 0; index < keys->count; ++index) {
    const cjwt_jwk_t *key = keys->keys[index];
    if (!key || !jwt_jwks_text_valid(key->kid, FLOWIE_SECURITY_ID_MAX) || !key->use ||
        strcmp(key->use, "sig") != 0 || !key->alg ||
        strcmp(key->alg, authenticator->algorithm) != 0 ||
        !jwt_jwks_key_type_matches(authenticator->algorithm_id, key->kty) ||
        jwt_jwks_key_has_private_material(key) || !jwt_jwks_public_material_valid(key))
      return SALTS_EPROTO;
    for (int previous = 0; previous < index; ++previous)
      if (strcmp(keys->keys[previous]->kid, key->kid) == 0) return SALTS_EPROTO;
  }
  return SALTS_OK;
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
  if (!claims || json_type(claims) != JSON_OBJECT || !name) return 0;
  for (size_t index = 0u; index < json_object_size(claims); ++index) {
    const char *key = json_object_key(claims, index);
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
    return SALTS_EPROTO;
  value = json_object_get(claims, name);
  if (!value || json_type(value) != JSON_STRING) return SALTS_EPROTO;
  text = json_string(value);
  length = json_string_len(value);
  if (!text || length == 0u || length >= capacity || memchr(text, '\0', length))
    return SALTS_EPROTO;
  memcpy(destination, text, length);
  destination[length] = '\0';
  return jwt_jwks_text_valid(destination, capacity - 1u) ? SALTS_OK : SALTS_EPROTO;
}

static int jwt_jwks_claim_u64(const json_value_t *claims, const char *name, uint64_t *out) {
  json_value_t *value;
  double number;
  uint64_t converted;
  if (out) *out = 0u;
  if (!claims || !name || !out || jwt_jwks_claim_occurrences(claims, name) != 1)
    return SALTS_EPROTO;
  value = json_object_get(claims, name);
  if (!value || json_type(value) != JSON_NUMBER) return SALTS_EPROTO;
  number = json_number(value);
  if (!isfinite(number) || number < 1.0 || number > 9007199254740991.0) return SALTS_EPROTO;
  converted = (uint64_t)number;
  if ((double)converted != number) return SALTS_EPROTO;
  *out = converted;
  return SALTS_OK;
}

static int jwt_jwks_claim_enabled(const json_value_t *claims) {
  json_value_t *value;
  if (!claims || jwt_jwks_claim_occurrences(claims, "account_enabled") != 1) return SALTS_EPROTO;
  value = json_object_get(claims, "account_enabled");
  if (!value || json_type(value) != JSON_BOOL) return SALTS_EPROTO;
  return json_bool(value) ? SALTS_OK : SALTS_EPERM;
}

static int jwt_jwks_copy_groups(const json_value_t *claims,
                                flowie_control_external_auth_assertion_t *assertion) {
  json_value_t *groups;
  size_t count;
  if (!claims || !assertion) return SALTS_EPROTO;
  if (jwt_jwks_claim_occurrences(claims, "groups") == 0) return SALTS_OK;
  if (jwt_jwks_claim_occurrences(claims, "groups") != 1) return SALTS_EPROTO;
  groups = json_object_get(claims, "groups");
  if (!groups || json_type(groups) != JSON_ARRAY) return SALTS_EPROTO;
  count = json_array_size(groups);
  if (count > FLOWIE_SECURITY_MAX_GROUPS) return SALTS_EPROTO;
  for (size_t index = 0u; index < count; ++index) {
    json_value_t *entry = json_array_get(groups, index);
    const char *text;
    size_t length;
    if (!entry || json_type(entry) != JSON_STRING) return SALTS_EPROTO;
    text = json_string(entry);
    length = json_string_len(entry);
    if (!text || length == 0u || length > FLOWIE_SECURITY_ID_MAX || memchr(text, '\0', length))
      return SALTS_EPROTO;
    memcpy(assertion->external_groups[index], text, length);
    assertion->external_groups[index][length] = '\0';
    if (!jwt_jwks_text_valid(assertion->external_groups[index], FLOWIE_SECURITY_ID_MAX))
      return SALTS_EPROTO;
    for (size_t previous = 0u; previous < index; ++previous)
      if (strcmp(assertion->external_groups[previous], assertion->external_groups[index]) == 0)
        return SALTS_EPROTO;
  }
  assertion->external_group_count = (uint32_t)count;
  return SALTS_OK;
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
    return SALTS_EPERM;
  if ((uint64_t)*token->iat > now) return SALTS_EPERM;
  rc = jwt_jwks_claim_text(token->private_claims, "domain_id", assertion.domain_id,
                           sizeof(assertion.domain_id));
  if (rc != SALTS_OK) return rc;
  if (request->domain_id[0] && strcmp(assertion.domain_id, request->domain_id) != 0)
    return SALTS_EPERM;
  rc = jwt_jwks_claim_enabled(token->private_claims);
  if (rc != SALTS_OK) return rc;
  rc = jwt_jwks_claim_u64(token->private_claims, "revision", &assertion.revision);
  if (rc != SALTS_OK) return rc;
  rc = jwt_jwks_claim_u64(token->private_claims, "assurance_level", &assurance);
  if (rc != SALTS_OK || assurance < FLOWIE_CONTROL_EXTERNAL_ASSURANCE_SINGLE_FACTOR ||
      assurance > FLOWIE_CONTROL_EXTERNAL_ASSURANCE_HARDWARE_BOUND)
    return SALTS_EPROTO;
  rc = jwt_jwks_copy_groups(token->private_claims, &assertion);
  if (rc != SALTS_OK) return rc;
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
  return SALTS_OK;
}

static int jwt_jwks_snapshot_parse(const flowie_control_jwt_jwks_authenticator_t *authenticator,
                                   const char *jwks_json, size_t jwks_size, uint64_t valid_until,
                                   flowie_control_jwt_jwks_snapshot_t **out) {
  flowie_control_jwt_jwks_snapshot_t *next = NULL;
  char *document = NULL;
  int rc = SALTS_EPROTO;
  if (out) *out = NULL;
  if (!authenticator || !jwks_json || jwks_size == 0u || !out ||
      jwks_size > authenticator->max_response_size || valid_until == 0u ||
      memchr(jwks_json, '\0', jwks_size))
    return SALTS_EINVAL;
  document = (char *)malloc(jwks_size + 1u);
  next = (flowie_control_jwt_jwks_snapshot_t *)calloc(1u, sizeof(*next));
  if (!document || !next) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  memcpy(document, jwks_json, jwks_size);
  document[jwks_size] = '\0';
  if (cjwt_jwks_parse(document, &next->keys) != CJWTE_OK || !next->keys) goto done;
  rc = jwt_jwks_snapshot_validate(authenticator, next->keys);
  if (rc != SALTS_OK) goto done;
  next->valid_until = valid_until;
  *out = next;
  next = NULL;
  rc = SALTS_OK;

done:
  free(document);
  jwt_jwks_snapshot_destroy(next);
  return rc;
}

static void jwt_jwks_snapshot_replace(flowie_control_jwt_jwks_authenticator_t *authenticator,
                                      flowie_control_jwt_jwks_snapshot_t *next) {
  flowie_control_jwt_jwks_snapshot_t *previous;
  salts_rwlock_wrlock(&authenticator->snapshot_lock);
  previous = authenticator->snapshot;
  authenticator->snapshot = next;
  salts_rwlock_wrunlock(&authenticator->snapshot_lock);
  jwt_jwks_snapshot_destroy(previous);
}

int flowie_control_jwt_jwks_authenticator_install(
    flowie_control_jwt_jwks_authenticator_t *authenticator, const char *jwks_json, size_t jwks_size,
    uint64_t valid_until) {
  flowie_control_jwt_jwks_snapshot_t *next = NULL;
  int rc = jwt_jwks_snapshot_parse(authenticator, jwks_json, jwks_size, valid_until, &next);
  if (rc != SALTS_OK) return rc;
  jwt_jwks_snapshot_replace(authenticator, next);
  return SALTS_OK;
}

int flowie_control_jwt_jwks_authenticator_verify_token(
    flowie_control_jwt_jwks_authenticator_t *authenticator,
    const flowie_control_external_auth_request_t *request, uint64_t now,
    flowie_control_external_auth_assertion_t *assertion_out) {
  flowie_control_external_auth_assertion_t empty = FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  int rc = SALTS_EPERM;
  if (assertion_out && assertion_out->size >= sizeof(*assertion_out)) *assertion_out = empty;
  if (!authenticator || !request || request->size < sizeof(*request) || !assertion_out ||
      assertion_out->size < sizeof(*assertion_out) || now == 0u ||
      now > (uint64_t)INT64_MAX - (uint64_t)authenticator->clock_skew_seconds ||
      !jwt_jwks_expected_domain_valid(request->domain_id) ||
      !jwt_jwks_text_valid(request->presented_identity, FLOWIE_SECURITY_ID_MAX) ||
      !request->method || strcmp(request->method, authenticator->method) != 0 || !request->secret ||
      request->secret_size == 0u || request->secret_size > authenticator->max_token_size ||
      memchr(request->secret, '\0', request->secret_size))
    return SALTS_EINVAL;
  salts_rwlock_rdlock(&authenticator->snapshot_lock);
  if (!authenticator->snapshot || authenticator->snapshot->valid_until <= now) {
    salts_rwlock_rdunlock(&authenticator->snapshot_lock);
    return SALTS_EBUSY;
  }
  for (int index = 0; index < authenticator->snapshot->keys->count; ++index) {
    const cjwt_jwk_t *key = authenticator->snapshot->keys->keys[index];
    cjwt_t *token = NULL;
    cjwt_code_t decoded =
        cjwt_decode_with_jwk((const char *)request->secret, request->secret_size, 0u, key,
                             (int64_t)now, (int64_t)authenticator->clock_skew_seconds, &token);
    if (decoded == CJWTE_OUT_OF_MEMORY) {
      rc = SALTS_ENOMEM;
      break;
    }
    if (decoded != CJWTE_OK || !token) continue;
    if (!token->header.kid || strcmp(token->header.kid, key->kid) != 0 ||
        token->header.alg != authenticator->algorithm_id) {
      cjwt_destroy(token);
      rc = SALTS_EPERM;
      break;
    }
    rc = jwt_jwks_assertion_from_token(authenticator, request, token, now, assertion_out);
    cjwt_destroy(token);
    break;
  }
  salts_rwlock_rdunlock(&authenticator->snapshot_lock);
  if (rc != SALTS_OK) *assertion_out = empty;
  return rc;
}

static int jwt_jwks_content_type_json(const chttp_response *response) {
  static const char media_type[] = "application/json";
  const char *content_type = chttp_response_header(response, "Content-Type");
  size_t index;
  if (!content_type) return 0;
  for (index = 0u; index < sizeof(media_type) - 1u; ++index) {
    unsigned char byte = (unsigned char)content_type[index];
    if (byte >= 'A' && byte <= 'Z') byte = (unsigned char)(byte + ('a' - 'A'));
    if (byte != (unsigned char)media_type[index]) return 0;
  }
  return content_type[index] == '\0' || content_type[index] == ';';
}

static native_io_backend_kind jwt_jwks_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_client_config
jwt_jwks_client_config(const flowie_control_jwt_jwks_authenticator_t *authenticator) {
  return (chttp_client_config){
      .network = {.backend = jwt_jwks_backend(),
                  .connection_capacity = 1u,
                  .command_capacity = 8u,
                  .request_capacity = 4u,
                  .completion_batch_capacity = 4u,
                  .event_capacity = 8u,
                  .max_send_bytes = JWT_JWKS_HEADER_LIMIT + 2048u,
                  .receive_buffer_bytes = 65536u,
                  .connect_timeout_ms = authenticator->timeout_ms,
                  .read_timeout_ms = authenticator->timeout_ms,
                  .write_timeout_ms = authenticator->timeout_ms,
                  .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                  .tls_handshake_timeout_ms = authenticator->timeout_ms},
      .request_capacity = 1u,
      .max_start_line_bytes = 2048u,
      .max_header_count = 16u,
      .max_header_bytes = JWT_JWKS_HEADER_LIMIT,
      .max_request_body_bytes = 1u,
      .max_response_body_bytes = authenticator->max_response_size,
      .max_informational_responses = 4u};
}

static int jwt_jwks_endpoint_text(char *connection_uri, size_t connection_capacity,
                                  char *authority, size_t authority_capacity, const char *host,
                                  uint16_t port) {
  const int ipv6_literal = host && strchr(host, ':') != NULL;
  int connection_size;
  int authority_size;
  if (!connection_uri || connection_capacity == 0u || !authority || authority_capacity == 0u ||
      !host || !host[0])
    return SALTS_EINVAL;
  connection_size = snprintf(connection_uri, connection_capacity,
                             ipv6_literal ? "tls://[%s]:%u" : "tls://%s:%u", host,
                             (unsigned int)port);
  if (port == 443u)
    authority_size = snprintf(authority, authority_capacity, ipv6_literal ? "[%s]" : "%s", host);
  else
    authority_size = snprintf(authority, authority_capacity,
                              ipv6_literal ? "[%s]:%u" : "%s:%u", host,
                              (unsigned int)port);
  if (connection_size <= 0 || (size_t)connection_size >= connection_capacity ||
      authority_size <= 0 || (size_t)authority_size >= authority_capacity)
    return SALTS_ERANGE;
  return SALTS_OK;
}

static int jwt_jwks_parse_offloaded(flowie_control_jwt_jwks_authenticator_t *authenticator,
                                    const char *json, size_t json_size, uint64_t valid_until,
                                    flowie_control_jwt_jwks_snapshot_t **snapshot_out) {
  flowie_control_jwt_jwks_parse_job_t *job;
  int rc;
  if (snapshot_out) *snapshot_out = NULL;
  if (!authenticator || !json || json_size == 0u || !snapshot_out ||
      json_size > SIZE_MAX - sizeof(*job) - 1u)
    return SALTS_EINVAL;
  job = (flowie_control_jwt_jwks_parse_job_t *)calloc(1u, sizeof(*job) + json_size + 1u);
  if (!job) return SALTS_ENOMEM;
  salts_mutex_init(&job->mutex);
  salts_cond_init(&job->completed_changed);
  job->authenticator = authenticator;
  job->valid_until = valid_until;
  job->json_size = json_size;
  job->result = SALTS_EIO;
  memcpy(job->json, json, json_size);
  job->json[json_size] = '\0';
  atomic_init(&job->references, 2u);
  if (salts_threadpool_try_submit(authenticator->executor, jwt_jwks_parse_job_run, job) !=
      SALTS_OK) {
    jwt_jwks_parse_job_release(job);
    jwt_jwks_parse_job_release(job);
    return SALTS_EBUSY;
  }
  salts_mutex_lock(&job->mutex);
  rc = jwt_jwks_wait(&job->completed_changed, &job->mutex, &job->completed,
                     authenticator->executor_deadline_ms);
  if (rc == SALTS_OK) {
    rc = job->result;
    if (rc == SALTS_OK) {
      *snapshot_out = job->snapshot;
      job->snapshot = NULL;
    }
  }
  salts_mutex_unlock(&job->mutex);
  jwt_jwks_parse_job_release(job);
  return rc;
}

static int jwt_jwks_fetch_snapshot(flowie_control_jwt_jwks_authenticator_t *authenticator,
                                   uint64_t now) {
  chttp_client client = {0};
  chttp_tls_profile tls_profile = {0};
  chttp_response response = {0};
  chttp_error error = {0};
  chttp_client_config client_config;
  chttp_options options;
  cnet_tls_client_config tls = {0};
  chttp_header headers[] = {{"Accept", "application/json"}};
  flowie_control_jwt_jwks_snapshot_t *snapshot = NULL;
  uri_t uri = {0};
  char connection_uri[320];
  char authority[320];
  uint64_t valid_until;
  int rc = SALTS_EIO;
  if (!authenticator || now == 0u || now > UINT64_MAX - authenticator->refresh_interval_seconds)
    return SALTS_EINVAL;
  valid_until = now + authenticator->refresh_interval_seconds;
  if (!uri_parse(authenticator->url, &uri) || !uri.valid) {
    rc = SALTS_EINVAL;
    goto done;
  }
  rc = jwt_jwks_endpoint_text(connection_uri, sizeof(connection_uri), authority,
                              sizeof(authority), authenticator->host, authenticator->port);
  if (rc != SALTS_OK) goto done;
  tls = (cnet_tls_client_config){.size = sizeof(tls),
                                 .ca_file = authenticator->ca_file,
                                 .server_name = authenticator->host};
  rc = chttp_tls_profile_init(&tls_profile, &tls);
  if (rc != SALTS_OK) goto done;
  client_config = jwt_jwks_client_config(authenticator);
  rc = chttp_client_init(&client, &client_config);
  if (rc != SALTS_OK) goto done;
  options = (chttp_options){.connection_uri = connection_uri,
                            .authority = authority,
                            .target = uri.path,
                            .headers = headers,
                            .header_count = sizeof(headers) / sizeof(headers[0]),
                            .timeout_ms = authenticator->timeout_ms,
                            .tls = &tls_profile,
                            .protocol = CHTTP_HTTP_1_1};
  rc = chttp_get(&client, &options, &response, &error);
  if (rc != SALTS_OK) {
    rc = SALTS_EIO;
    goto done;
  }
  if (response.status_code == 429u) {
    rc = SALTS_EBUSY;
    goto done;
  }
  if (response.status_code != 200u || !response.body || response.body_size == 0u ||
      response.body_size > authenticator->max_response_size ||
      !jwt_jwks_content_type_json(&response)) {
    rc = SALTS_EPROTO;
    goto done;
  }
  rc = jwt_jwks_parse_offloaded(authenticator, (const char *)response.body, response.body_size,
                                valid_until,
                                &snapshot);
  if (rc != SALTS_OK) goto done;
  jwt_jwks_snapshot_replace(authenticator, snapshot);
  snapshot = NULL;

done:
  jwt_jwks_snapshot_destroy(snapshot);
  chttp_response_destroy(&response);
  if (client.impl != NULL)
    (void)chttp_client_destroy(&client, JWT_JWKS_DESTROY_TIMEOUT_MS);
  (void)chttp_tls_profile_destroy(&tls_profile);
  return rc;
}

static int jwt_jwks_refresh_if_needed(flowie_control_jwt_jwks_authenticator_t *authenticator,
                                      uint64_t now) {
  int expected = 0;
  int current = 0;
  int rc;
  if (!authenticator || now == 0u) return SALTS_EINVAL;
  salts_rwlock_rdlock(&authenticator->snapshot_lock);
  if (authenticator->snapshot && authenticator->snapshot->valid_until > now) current = 1;
  salts_rwlock_rdunlock(&authenticator->snapshot_lock);
  if (current) return SALTS_OK;
  if (!atomic_compare_exchange_strong_explicit(&authenticator->refresh_in_flight, &expected, 1,
                                               memory_order_acq_rel, memory_order_acquire))
    return SALTS_EBUSY;
  salts_rwlock_rdlock(&authenticator->snapshot_lock);
  if (authenticator->snapshot && authenticator->snapshot->valid_until > now) current = 1;
  salts_rwlock_rdunlock(&authenticator->snapshot_lock);
  rc = current ? SALTS_OK : jwt_jwks_fetch_snapshot(authenticator, now);
  atomic_store_explicit(&authenticator->refresh_in_flight, 0, memory_order_release);
  return rc;
}

static int jwt_jwks_verify(void *ctx, const flowie_control_external_auth_request_t *request,
                           flowie_control_external_auth_assertion_t *assertion_out) {
  flowie_control_jwt_jwks_authenticator_t *authenticator =
      (flowie_control_jwt_jwks_authenticator_t *)ctx;
  flowie_control_jwt_jwks_verify_job_t *job;
  uint64_t now;
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
    return SALTS_EINVAL;
  now = authenticator->clock_seconds(authenticator->clock_ctx);
  if (now == 0u || now > (uint64_t)INT64_MAX - (uint64_t)authenticator->clock_skew_seconds)
    return SALTS_EIO;
  rc = jwt_jwks_refresh_if_needed(authenticator, now);
  if (rc != SALTS_OK) return rc;
  if (request->secret_size > SIZE_MAX - sizeof(*job)) return SALTS_ERANGE;
  job = (flowie_control_jwt_jwks_verify_job_t *)calloc(1u, sizeof(*job) + request->secret_size);
  if (!job) return SALTS_ENOMEM;
  salts_mutex_init(&job->mutex);
  salts_cond_init(&job->completed_changed);
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
  job->result = SALTS_EIO;
  atomic_init(&job->references, 2u);
  if (salts_threadpool_try_submit(authenticator->executor, jwt_jwks_verify_job_run, job) !=
      SALTS_OK) {
    jwt_jwks_verify_job_release(job);
    jwt_jwks_verify_job_release(job);
    return SALTS_EBUSY;
  }
  salts_mutex_lock(&job->mutex);
  rc = jwt_jwks_wait(&job->completed_changed, &job->mutex, &job->completed,
                     authenticator->executor_deadline_ms);
  if (rc == SALTS_OK) {
    rc = job->result;
    if (rc == SALTS_OK) *assertion_out = job->assertion;
  }
  salts_mutex_unlock(&job->mutex);
  jwt_jwks_verify_job_release(job);
  return rc;
}

int flowie_control_jwt_jwks_authenticator_create(
    const flowie_control_jwt_jwks_authenticator_config_t *config,
    flowie_control_jwt_jwks_authenticator_t **out) {
  flowie_control_jwt_jwks_authenticator_t *authenticator = NULL;
  cjwt_alg_t algorithm = alg_none;
  int rc = SALTS_ENOMEM;
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
    return SALTS_EINVAL;
  authenticator = (flowie_control_jwt_jwks_authenticator_t *)calloc(1u, sizeof(*authenticator));
  if (!authenticator) return SALTS_ENOMEM;
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
  if (rc != SALTS_OK) goto fail;
  rc = jwt_jwks_ca_file_validate(authenticator->ca_file);
  if (rc != SALTS_OK) goto fail;
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
  if (salts_rwlock_init(&authenticator->snapshot_lock) != SALTS_OK) goto fail;
  authenticator->snapshot_lock_initialized = 1;
  {
    salts_threadpool_config_t executor_config = {(int)config->executor_workers,
                                                 config->executor_queue_capacity};
    authenticator->executor = salts_threadpool_create_with_config(&executor_config);
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
  return SALTS_OK;

fail:
  flowie_control_jwt_jwks_authenticator_destroy(authenticator);
  return rc;
}

void flowie_control_jwt_jwks_authenticator_destroy(
    flowie_control_jwt_jwks_authenticator_t *authenticator) {
  if (!authenticator) return;
  salts_threadpool_destroy(authenticator->executor);
  authenticator->executor = NULL;
  jwt_jwks_snapshot_destroy(authenticator->snapshot);
  authenticator->snapshot = NULL;
  if (authenticator->snapshot_lock_initialized) salts_rwlock_destroy(&authenticator->snapshot_lock);
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
