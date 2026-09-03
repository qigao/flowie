#include "flowie_control_auth_iris_endpoint_internal.h"
#include "flowie_control_async_internal.h"

#include "base64_utils.h"
#include "monocypher.h"
#include "salts_error.h"
#include "salts_thread.h"
#include <json_parser.h>

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define FLOWIE_CONTROL_AUTH_HTTP_TOKEN_MAX 4096u
#define FLOWIE_CONTROL_AUTH_HTTP_AUTHORIZATION_MAX                                                 \
  (sizeof("Bearer ") - 1u + FLOWIE_CONTROL_AUTH_HTTP_TOKEN_MAX)

typedef struct flowie_control_auth_local_job_s {
  flowie_control_auth_iris_adapter_t *adapter;
  atomic_uint references;
  atomic_int completed;
  flowie_control_verified_caller_t caller;
  char listener_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char service_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char peer_certificate_sha256[FLOWIE_CONTROL_HTTP_PEER_CERTIFICATE_SHA256_CAPACITY];
  flowie_control_auth_http_request_t request;
  flowie_security_principal_t principal;
  int result;
} flowie_control_auth_local_job_t;

struct flowie_control_auth_iris_endpoint_s {
  flowie_control_auth_iris_adapter_t *adapter;
  flowie_control_service_credential_resolver_t *service_credentials;
  size_t max_request_body_size;
  size_t max_secret_size;
  salts_threadpool_t *local_executor;
  uint32_t local_executor_deadline_ms;
  flowie_control_http_app_t *bound_app;
};

static void flowie_control_auth_principal_init(flowie_security_principal_t *principal) {
  if (!principal) return;
  memset(principal, 0, sizeof(*principal));
  principal->size = sizeof(*principal);
  principal->abi_version = FLOWIE_SECURITY_ABI_V3;
}

static void flowie_control_auth_local_job_release(flowie_control_auth_local_job_t *job) {
  if (!job || atomic_fetch_sub_explicit(&job->references, 1u, memory_order_acq_rel) != 1u) return;
  crypto_wipe(job, sizeof(*job));
  free(job);
}

static void flowie_control_auth_local_job_run(void *arg) {
  flowie_control_auth_local_job_t *job = (flowie_control_auth_local_job_t *)arg;
  if (!job) return;
  flowie_control_auth_principal_init(&job->principal);
  job->result = flowie_control_auth_iris_adapter_authenticate_verified(
      job->adapter, &job->caller, job->request.identity, job->request.method,
      job->request.secret, job->request.secret_size, job->request.protocol,
      job->request.remote_address,
      job->request.peer_certificate_sha256[0] != '\0' ? job->request.peer_certificate_sha256 : NULL,
      &job->principal, NULL);
  atomic_store_explicit(&job->completed, 1, memory_order_release);
  flowie_control_auth_local_job_release(job);
}

static int flowie_control_auth_http_ascii_equal(const char *left, const char *right) {
  if (!left || !right) return 0;
  while (*left && *right) {
    if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) return 0;
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static int flowie_control_auth_http_text_valid(const char *value, size_t maximum) {
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

static void flowie_control_auth_http_free_value(json_value_t *value) {
  json_value_t *owned = (json_value_t *)value;
  if (owned) json_free(owned);
}

static int flowie_control_auth_http_add(json_value_t *object, const char *field,
                                        json_value_t *value) {
  if (value && json_object_add_checked(object, field, value)) return SALTS_OK;
  flowie_control_auth_http_free_value(value);
  return SALTS_ENOMEM;
}

static int flowie_control_auth_http_array_add(json_value_t *array, json_value_t *value) {
  if (value && json_array_add_checked(array, value)) return SALTS_OK;
  flowie_control_auth_http_free_value(value);
  return SALTS_ENOMEM;
}

static int flowie_control_auth_http_fields_exact(const json_value_t *object,
                                                 const char *const *allowed, size_t allowed_count) {
  if (!object || json_type(object) != JSON_OBJECT ||
      json_object_size(object) != allowed_count)
    return SALTS_EPROTO;
  for (size_t index = 0u; index < json_object_size(object); ++index) {
    const char *field = json_object_key(object, index);
    int known = 0;
    for (size_t allowed_index = 0u; allowed_index < allowed_count; ++allowed_index) {
      if (field && strcmp(field, allowed[allowed_index]) == 0) {
        known = 1;
        break;
      }
    }
    if (!known) return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int flowie_control_auth_http_json_u64(const json_value_t *value, uint64_t *out) {
  const char *text;
  char buffer[32];
  char *end = NULL;
  size_t size = 0u;
  unsigned long long parsed;
  if (!value || json_type(value) != JSON_NUMBER || !out) return SALTS_EPROTO;
  text = json_number_text(value, &size);
  if (!text || size == 0u || size >= sizeof(buffer)) return SALTS_EPROTO;
  memcpy(buffer, text, size);
  buffer[size] = '\0';
  if (buffer[0] == '-' || buffer[0] == '+' || (size > 1u && buffer[0] == '0')) return SALTS_EPROTO;
  parsed = strtoull(buffer, &end, 10);
  if (!end || *end != '\0') return SALTS_EPROTO;
  *out = (uint64_t)parsed;
  return SALTS_OK;
}

static int flowie_control_auth_http_copy_string(const json_value_t *object, const char *field,
                                                char *output, size_t capacity, int required) {
  json_value_t *value;
  const char *text;
  size_t size;
  if (!object || !field || !output || capacity == 0u) return SALTS_EPROTO;
  value = json_object_get(object, field);
  if (!value || json_type(value) != JSON_STRING) return SALTS_EPROTO;
  text = json_string(value);
  size = json_string_len(value);
  if (!text || (required && size == 0u) || size >= capacity || memchr(text, '\0', size))
    return SALTS_EPROTO;
  memcpy(output, text, size);
  output[size] = '\0';
  return SALTS_OK;
}

static int flowie_control_auth_http_fingerprint_valid(const char *value) {
  static const char prefix[] = "sha256:";
  size_t size;
  if (!value || value[0] == '\0') return 1;
  size = strlen(value);
  if (size != FLOWIE_CONTROL_AUTH_CERT_SHA256_TEXT_SIZE ||
      memcmp(value, prefix, sizeof(prefix) - 1u) != 0)
    return 0;
  for (size_t i = sizeof(prefix) - 1u; i < size; ++i)
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return 0;
  return 1;
}

void flowie_control_auth_http_request_clear(flowie_control_auth_http_request_t *request) {
  if (request) crypto_wipe(request, sizeof(*request));
}

int flowie_control_auth_http_decode_request(const char *body, size_t body_size,
                                            size_t max_secret_size,
                                            flowie_control_auth_http_request_t *request_out) {
  static const char *const allowed[] = {"version",
                                        "identity",
                                        "method",
                                        "secret_base64",
                                        "protocol",
                                        "remote_address",
                                        "peer_certificate_sha256"};
  json_value_t *document = NULL;
  json_value_t *encoded_value;
  const char *encoded;
  uint8_t *decoded = NULL;
  char *canonical = NULL;
  size_t encoded_size;
  size_t decoded_size = 0u;
  uint64_t version = 0u;
  int rc = SALTS_EPROTO;

  if (!request_out) return SALTS_EINVAL;
  memset(request_out, 0, sizeof(*request_out));
  if (!body || body_size == 0u || body_size > FLOWIE_CONTROL_AUTH_HTTP_ABSOLUTE_REQUEST_BODY_MAX ||
      max_secret_size == 0u || max_secret_size > FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX)
    return SALTS_EPROTO;
  document = json_parse(body, body_size);
  if (!document)
    return SALTS_EPROTO;
  if (flowie_control_auth_http_fields_exact(document, allowed,
                                            sizeof(allowed) / sizeof(allowed[0])) != SALTS_OK ||
      flowie_control_auth_http_json_u64(json_object_get(document, "version"), &version) !=
          SALTS_OK ||
      version != FLOWIE_CONTROL_AUTH_HTTP_PROTOCOL_VERSION ||
      flowie_control_auth_http_copy_string(document, "identity", request_out->identity,
                                           sizeof(request_out->identity), 1) != SALTS_OK ||
      flowie_control_auth_http_copy_string(document, "method", request_out->method,
                                           sizeof(request_out->method), 1) != SALTS_OK ||
      flowie_control_auth_http_copy_string(document, "protocol", request_out->protocol,
                                           sizeof(request_out->protocol), 1) != SALTS_OK ||
      flowie_control_auth_http_copy_string(document, "remote_address", request_out->remote_address,
                                           sizeof(request_out->remote_address), 1) != SALTS_OK ||
      flowie_control_auth_http_copy_string(
          document, "peer_certificate_sha256", request_out->peer_certificate_sha256,
          sizeof(request_out->peer_certificate_sha256), 0) != SALTS_OK ||
      !flowie_control_auth_http_fingerprint_valid(request_out->peer_certificate_sha256))
    goto done;

  encoded_value = json_object_get(document, "secret_base64");
  if (!encoded_value || json_type(encoded_value) != JSON_STRING) goto done;
  encoded = json_string(encoded_value);
  encoded_size = json_string_len(encoded_value);
  if (!encoded || encoded_size == 0u || encoded_size > ((max_secret_size + 2u) / 3u) * 4u ||
      memchr(encoded, '\0', encoded_size) || strlen(encoded) != encoded_size ||
      tn_base64_decode(encoded, &decoded, &decoded_size) != 0 || !decoded || decoded_size == 0u ||
      decoded_size > max_secret_size)
    goto done;
  if (tn_base64_encode(decoded, decoded_size, &canonical) != 0 || !canonical) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  if (strlen(canonical) != encoded_size || memcmp(canonical, encoded, encoded_size) != 0) goto done;
  memcpy(request_out->secret, decoded, decoded_size);
  request_out->secret_size = decoded_size;
  rc = SALTS_OK;

done:
  if (canonical) {
    crypto_wipe(canonical, strlen(canonical));
    free(canonical);
  }
  if (decoded) {
    crypto_wipe(decoded, decoded_size);
    free(decoded);
  }
  json_free(document);
  if (rc != SALTS_OK) flowie_control_auth_http_request_clear(request_out);
  return rc;
}

static const char *flowie_control_auth_http_scope_name(flowie_security_scope_t scope) {
  switch (scope) {
  case FLOWIE_SECURITY_SCOPE_SELF:
    return "self";
  case FLOWIE_SECURITY_SCOPE_GROUP:
    return "group";
  case FLOWIE_SECURITY_SCOPE_DOMAIN:
    return "domain";
  case FLOWIE_SECURITY_SCOPE_SYSTEM:
    return "system";
  default:
    return NULL;
  }
}

int flowie_control_auth_http_encode_principal(const flowie_security_principal_t *principal,
                                              char **body_out, size_t *body_size_out) {
  json_value_t *document = NULL;
  json_value_t *principal_json = NULL;
  json_value_t *roles = NULL;
  json_value_t *groups = NULL;
  const char *scope;
  int rc = SALTS_ENOMEM;

  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!principal || principal->size < sizeof(*principal) || !body_out || !body_size_out ||
      principal->role_count > FLOWIE_SECURITY_MAX_ROLES ||
      principal->group_count > FLOWIE_SECURITY_MAX_GROUPS || principal->policy_version == 0u ||
      !(scope = flowie_control_auth_http_scope_name(principal->scope)) ||
      !flowie_control_auth_http_text_valid(principal->principal_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_auth_http_text_valid(principal->principal_type,
                                           FLOWIE_SECURITY_TYPE_MAX) ||
      !flowie_control_auth_http_text_valid(principal->domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_auth_http_text_valid(principal->auth_method, FLOWIE_SECURITY_TYPE_MAX))
    return SALTS_EINVAL;

  document = (json_value_t *)json_create_object();
  principal_json = json_create_object();
  roles = json_create_array();
  groups = json_create_array();
  if (!document || !principal_json || !roles || !groups) goto done;
  for (uint32_t index = 0u; index < principal->role_count; ++index) {
    if (!flowie_control_auth_http_text_valid(principal->roles[index],
                                             FLOWIE_SECURITY_TYPE_MAX)) {
      rc = SALTS_EPROTO;
      goto done;
    }
    if (flowie_control_auth_http_array_add(
            roles, json_create_string(principal->roles[index])) != SALTS_OK)
      goto done;
  }
  for (uint32_t index = 0u; index < principal->group_count; ++index) {
    if (!flowie_control_auth_http_text_valid(principal->groups[index],
                                             FLOWIE_SECURITY_ID_MAX)) {
      rc = SALTS_EPROTO;
      goto done;
    }
    if (flowie_control_auth_http_array_add(
            groups, json_create_string(principal->groups[index])) != SALTS_OK)
      goto done;
  }
  if (flowie_control_auth_http_add(
          document, "version",
          json_create_uint64(FLOWIE_CONTROL_AUTH_HTTP_PROTOCOL_VERSION)) != SALTS_OK ||
      flowie_control_auth_http_add(document, "authenticated", json_create_bool(true)) !=
          SALTS_OK ||
      flowie_control_auth_http_add(principal_json, "id",
                                   json_create_string(principal->principal_id)) != SALTS_OK ||
      flowie_control_auth_http_add(principal_json, "type",
                                   json_create_string(principal->principal_type)) !=
          SALTS_OK ||
      flowie_control_auth_http_add(principal_json, "domain",
                                   json_create_string(principal->domain_id)) !=
          SALTS_OK ||
      flowie_control_auth_http_add(principal_json, "auth_method",
                                   json_create_string(principal->auth_method)) != SALTS_OK ||
      flowie_control_auth_http_add(principal_json, "scope", json_create_string(scope)) !=
          SALTS_OK)
    goto done;
  if (flowie_control_auth_http_add(principal_json, "roles", roles) != SALTS_OK) {
    roles = NULL;
    goto done;
  }
  roles = NULL;
  if (flowie_control_auth_http_add(principal_json, "groups", groups) != SALTS_OK) {
    groups = NULL;
    goto done;
  }
  groups = NULL;
  if (flowie_control_auth_http_add(principal_json, "expires_at",
                                   json_create_uint64(principal->expires_at)) != SALTS_OK ||
      flowie_control_auth_http_add(principal_json, "policy_version",
                                   json_create_uint64(principal->policy_version)) != SALTS_OK)
    goto done;
  if (flowie_control_auth_http_add(document, "principal", principal_json) != SALTS_OK) {
    principal_json = NULL;
    goto done;
  }
  principal_json = NULL;
  *body_out = json_serialize(document, body_size_out);
  if (!*body_out) goto done;
  if (*body_size_out > FLOWIE_CONTROL_AUTH_HTTP_ABSOLUTE_REQUEST_BODY_MAX) {
    json_serialize_free(*body_out);
    *body_out = NULL;
    *body_size_out = 0u;
    rc = SALTS_EFBIG;
    goto done;
  }
  rc = SALTS_OK;

done:
  flowie_control_auth_http_free_value(roles);
  flowie_control_auth_http_free_value(groups);
  flowie_control_auth_http_free_value(principal_json);
  json_free(document);
  return rc;
}

static int flowie_control_auth_http_encode_denied(char **body_out, size_t *body_size_out) {
  json_value_t *document = NULL;
  int rc = SALTS_ENOMEM;
  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!body_out || !body_size_out) return SALTS_EINVAL;
  document = (json_value_t *)json_create_object();
  if (!document) return SALTS_ENOMEM;
  if (flowie_control_auth_http_add(
          document, "version",
          json_create_uint64(FLOWIE_CONTROL_AUTH_HTTP_PROTOCOL_VERSION)) == SALTS_OK &&
      flowie_control_auth_http_add(document, "authenticated", json_create_bool(false)) ==
          SALTS_OK) {
    *body_out = json_serialize(document, body_size_out);
    if (*body_out) rc = SALTS_OK;
  }
  json_free(document);
  return rc;
}

static int flowie_control_auth_http_header(const Req *req, const char *name,
                                           const char **value_out) {
  const char *found = NULL;
  size_t matches = 0u;
  if (value_out) *value_out = NULL;
  if (!req || !name || !value_out) return SALTS_EINVAL;
  if (req->headers.count > 0 && !req->headers.items) return SALTS_EPROTO;
  for (int index = 0; index < req->headers.count; ++index) {
    const request_item_t *item = &req->headers.items[index];
    if (item->key && flowie_control_auth_http_ascii_equal(item->key, name)) {
      ++matches;
      found = item->value;
    }
  }
  if (matches != 1u || !found) return SALTS_EPROTO;
  *value_out = found;
  return SALTS_OK;
}

static void flowie_control_auth_http_wipe_header(Req *req, const char *name) {
  if (!req || !name || req->headers.count <= 0 || !req->headers.items) return;
  for (int index = 0; index < req->headers.count; ++index) {
    request_item_t *item = &req->headers.items[index];
    if (item->key && item->value && flowie_control_auth_http_ascii_equal(item->key, name)) {
      size_t size = strnlen(item->value, FLOWIE_CONTROL_AUTH_HTTP_AUTHORIZATION_MAX + 1u);
      if (size <= FLOWIE_CONTROL_AUTH_HTTP_AUTHORIZATION_MAX) crypto_wipe(item->value, size);
    }
  }
}

static int flowie_control_auth_http_resolve_caller(
    flowie_control_auth_iris_endpoint_t *endpoint, const Req *req,
    flowie_control_verified_caller_t *caller_out) {
  static const char prefix[] = "Bearer ";
  const char *authorization = NULL;
  const char *service_domain = NULL;
  const char *service_id = NULL;
  size_t authorization_size;
  int rc;

  if (!endpoint || !caller_out || caller_out->size < sizeof(*caller_out)) return SALTS_EINVAL;
  rc = flowie_control_auth_http_header(req, "Authorization", &authorization);
  if (rc != SALTS_OK) return SALTS_EPERM;
  if (flowie_control_auth_http_header(req, "X-Flowie-Service-Domain", &service_domain) !=
          SALTS_OK ||
      flowie_control_auth_http_header(req, "X-Flowie-Service-Id", &service_id) != SALTS_OK)
    return SALTS_EPERM;
  authorization_size = strnlen(authorization, FLOWIE_CONTROL_AUTH_HTTP_AUTHORIZATION_MAX + 1u);
  if (authorization_size <= sizeof(prefix) - 1u ||
      authorization_size > FLOWIE_CONTROL_AUTH_HTTP_AUTHORIZATION_MAX ||
      memcmp(authorization, prefix, sizeof(prefix) - 1u) != 0)
    return SALTS_EPERM;
  return flowie_control_service_credential_resolve(
      endpoint->service_credentials, service_domain, service_id,
      (const uint8_t *)authorization + sizeof(prefix) - 1u,
      authorization_size - (sizeof(prefix) - 1u), FLOWIE_CONTROL_SERVICE_AUTHENTICATE,
      caller_out);
}

static int flowie_control_auth_http_response_status(int rc) {
  if (rc == SALTS_EPERM) return FORBIDDEN;
  if (rc == SALTS_EBUSY) return TOO_MANY_REQUESTS;
  return SERVICE_UNAVAILABLE;
}

int flowie_control_auth_iris_endpoint_authenticate_verified(
    flowie_control_auth_iris_endpoint_t *endpoint, const flowie_control_verified_caller_t *caller,
    const flowie_control_auth_http_request_t *request,
    flowie_security_principal_t *principal_out) {
  flowie_control_auth_local_job_t *job;
  int completed;
  int wait_rc;
  int rc;
  if (principal_out && principal_out->size >= sizeof(*principal_out))
    flowie_control_auth_principal_init(principal_out);
  if (!endpoint || !caller || caller->size < sizeof(*caller) || !request || !principal_out ||
      principal_out->size < sizeof(*principal_out) ||
      caller->authenticated != 1 ||
      !flowie_control_auth_http_text_valid(caller->listener_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_auth_http_text_valid(caller->service_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_auth_http_text_valid(caller->domain_id, FLOWIE_SECURITY_ID_MAX) ||
      (caller->peer_certificate_sha256 &&
       !flowie_control_auth_http_fingerprint_valid(caller->peer_certificate_sha256)) ||
      !flowie_control_auth_http_text_valid(request->identity, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_auth_http_text_valid(request->method, FLOWIE_SECURITY_TYPE_MAX) ||
      !flowie_control_auth_http_text_valid(request->protocol, FLOWIE_SECURITY_TYPE_MAX) ||
      !flowie_control_auth_http_text_valid(request->remote_address,
                                           FLOWIE_CONTROL_AUTH_HTTP_REMOTE_ADDRESS_MAX) ||
      !flowie_control_auth_http_fingerprint_valid(request->peer_certificate_sha256) ||
      request->secret_size == 0u || request->secret_size > endpoint->max_secret_size)
    return SALTS_EINVAL;

  if (!endpoint->local_executor)
    return flowie_control_auth_iris_adapter_authenticate_verified(
        endpoint->adapter, caller, request->identity, request->method, request->secret,
        request->secret_size, request->protocol, request->remote_address,
        request->peer_certificate_sha256[0] != '\0' ? request->peer_certificate_sha256 : NULL,
        principal_out, NULL);

  job = (flowie_control_auth_local_job_t *)calloc(1u, sizeof(*job));
  if (!job) return SALTS_ENOMEM;
  job->adapter = endpoint->adapter;
  job->caller = (flowie_control_verified_caller_t)FLOWIE_CONTROL_VERIFIED_CALLER_INIT;
  memcpy(job->listener_id, caller->listener_id, strlen(caller->listener_id) + 1u);
  memcpy(job->service_id, caller->service_id, strlen(caller->service_id) + 1u);
  memcpy(job->domain_id, caller->domain_id, strlen(caller->domain_id) + 1u);
  if (caller->peer_certificate_sha256)
    memcpy(job->peer_certificate_sha256, caller->peer_certificate_sha256,
           strlen(caller->peer_certificate_sha256) + 1u);
  job->caller.listener_id = job->listener_id;
  job->caller.service_id = job->service_id;
  job->caller.domain_id = job->domain_id;
  job->caller.peer_certificate_sha256 =
      job->peer_certificate_sha256[0] ? job->peer_certificate_sha256 : NULL;
  job->caller.permissions = caller->permissions;
  job->caller.authenticated = caller->authenticated;
  job->request = *request;
  flowie_control_auth_principal_init(&job->principal);
  job->result = SALTS_EIO;
  atomic_init(&job->references, 2u);
  atomic_init(&job->completed, 0);
  if (salts_threadpool_try_submit(endpoint->local_executor, flowie_control_auth_local_job_run,
                                  job) != SALTS_OK) {
    flowie_control_auth_local_job_release(job);
    flowie_control_auth_local_job_release(job);
    return SALTS_EBUSY;
  }
  wait_rc = flowie_control_async_wait(&job->completed, endpoint->local_executor_deadline_ms);
  completed = atomic_load_explicit(&job->completed, memory_order_acquire);
  if (completed) {
    rc = job->result;
    if (rc == SALTS_OK) *principal_out = job->principal;
  } else {
    rc = wait_rc == SALTS_OK ? SALTS_ETIMEDOUT : wait_rc;
  }
  flowie_control_auth_local_job_release(job);
  return rc;
}

int flowie_control_auth_iris_endpoint_process(flowie_control_auth_iris_endpoint_t *endpoint,
                                              Req *req, int *status_out, char **body_out,
                                              size_t *body_size_out) {
  flowie_control_auth_http_request_t request;
  flowie_security_principal_t principal;
  flowie_control_verified_caller_t caller = FLOWIE_CONTROL_VERIFIED_CALLER_INIT;
  char peer_certificate_sha256[FLOWIE_CONTROL_HTTP_PEER_CERTIFICATE_SHA256_CAPACITY];
  const char *content_type = NULL;
  int status = BAD_REQUEST;
  int rc = SALTS_EPROTO;

  memset(&request, 0, sizeof(request));
  flowie_control_auth_principal_init(&principal);
  memset(peer_certificate_sha256, 0, sizeof(peer_certificate_sha256));
  if (status_out) *status_out = INTERNAL_SERVER_ERROR;
  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!req || !status_out || !body_out || !body_size_out) return SALTS_EINVAL;
  if (!endpoint) {
    if (req->body && req->body_len > 0u) crypto_wipe(req->body, req->body_len);
    flowie_control_auth_http_wipe_header(req, "Authorization");
    return SALTS_EINVAL;
  }
  if (!req->method || strcmp(req->method, "POST") != 0 || req->body_stream || !req->body ||
      req->body_len == 0u || req->body_len > endpoint->max_request_body_size ||
      flowie_control_auth_http_header(req, "Content-Type", &content_type) != SALTS_OK ||
      !flowie_control_auth_http_ascii_equal(content_type, "application/json"))
    goto done;
  rc = flowie_control_auth_http_decode_request(req->body, req->body_len, endpoint->max_secret_size,
                                               &request);
  if (rc != SALTS_OK) {
    if (rc == SALTS_ENOMEM) status = SERVICE_UNAVAILABLE;
    goto done;
  }
  rc = flowie_control_auth_iris_adapter_optional_verified_peer_certificate(
      req, peer_certificate_sha256);
  if (rc != SALTS_OK) {
    status = flowie_control_auth_http_response_status(rc);
    goto done;
  }
  rc = flowie_control_auth_http_resolve_caller(endpoint, req, &caller);
  if (rc != SALTS_OK) {
    status = flowie_control_auth_http_response_status(rc);
    goto done;
  }
  rc = flowie_control_auth_iris_endpoint_authenticate_verified(endpoint, &caller, &request,
                                                               &principal);
  if (rc != SALTS_OK) {
    status = flowie_control_auth_http_response_status(rc);
    goto done;
  }
  rc = flowie_control_auth_http_encode_principal(&principal, body_out, body_size_out);
  status = rc == SALTS_OK ? OK : SERVICE_UNAVAILABLE;

done:
  flowie_control_auth_http_request_clear(&request);
  crypto_wipe(&principal, sizeof(principal));
  crypto_wipe(peer_certificate_sha256, sizeof(peer_certificate_sha256));
  if (req->body && req->body_len > 0u) crypto_wipe(req->body, req->body_len);
  flowie_control_auth_http_wipe_header(req, "Authorization");
  if (rc != SALTS_OK && !*body_out) {
    int encode_rc = flowie_control_auth_http_encode_denied(body_out, body_size_out);
    if (encode_rc != SALTS_OK) return encode_rc;
  }
  *status_out = status;
  return SALTS_OK;
}

void flowie_control_auth_iris_endpoint_handle(flowie_control_auth_iris_endpoint_t *endpoint,
                                              Req *req, Res *res) {
  static const char internal_error[] = "{\"version\":2,\"authenticated\":false}";
  char *body = NULL;
  size_t body_size = 0u;
  int status = INTERNAL_SERVER_ERROR;
  if (!res) return;
  set_header(res, "Cache-Control", "no-store");
  set_header(res, "Pragma", "no-cache");
  if (flowie_control_auth_iris_endpoint_process(endpoint, req, &status, &body, &body_size) !=
      SALTS_OK) {
    reply(res, INTERNAL_SERVER_ERROR, "application/json", internal_error,
          sizeof(internal_error) - 1u);
    return;
  }
  reply(res, status, "application/json", body, body_size);
  json_serialize_free(body);
}

static void flowie_control_auth_iris_registered_handler(Req *req, Res *res) {
  flowie_control_auth_iris_endpoint_t *endpoint = NULL;
  if (req && req->app && req->path && strcmp(req->path, FLOWIE_CONTROL_AUTH_HTTP_PATH) == 0)
    endpoint = (flowie_control_auth_iris_endpoint_t *)flowie_control_http_app_lookup_context(
        req->app, FLOWIE_CONTROL_AUTH_HTTP_PATH);
  flowie_control_auth_iris_endpoint_handle(endpoint, req, res);
}

int flowie_control_auth_iris_endpoint_create(
    const flowie_control_auth_iris_endpoint_config_t *config,
    flowie_control_auth_iris_endpoint_t **out) {
  flowie_control_auth_iris_endpoint_t *endpoint;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) || !out || !config->adapter ||
      !config->service_credentials || config->max_request_body_size == 0u ||
      config->max_request_body_size > FLOWIE_CONTROL_AUTH_HTTP_ABSOLUTE_REQUEST_BODY_MAX ||
      config->max_secret_size == 0u ||
      config->max_secret_size > FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX ||
      (config->local_executor_enabled &&
       (config->local_executor_workers == 0u ||
        config->local_executor_workers > FLOWIE_CONTROL_AUTH_LOCAL_EXECUTOR_MAX_WORKERS ||
        config->local_executor_queue_capacity == 0u ||
        config->local_executor_queue_capacity >
            FLOWIE_CONTROL_AUTH_LOCAL_EXECUTOR_MAX_QUEUE_CAPACITY ||
        config->local_executor_deadline_ms == 0u ||
        config->local_executor_deadline_ms > FLOWIE_CONTROL_AUTH_LOCAL_EXECUTOR_MAX_DEADLINE_MS)))
    return SALTS_EINVAL;
  endpoint = (flowie_control_auth_iris_endpoint_t *)calloc(1u, sizeof(*endpoint));
  if (!endpoint) return SALTS_ENOMEM;
  endpoint->adapter = config->adapter;
  endpoint->service_credentials = config->service_credentials;
  endpoint->max_request_body_size = config->max_request_body_size;
  endpoint->max_secret_size = config->max_secret_size;
  endpoint->local_executor_deadline_ms = config->local_executor_deadline_ms;
  if (config->local_executor_enabled) {
    salts_threadpool_config_t executor_config = {(int)config->local_executor_workers,
                                                 config->local_executor_queue_capacity};
    endpoint->local_executor = salts_threadpool_create_with_config(&executor_config);
    if (!endpoint->local_executor) {
      crypto_wipe(endpoint, sizeof(*endpoint));
      free(endpoint);
      return SALTS_ENOMEM;
    }
  }
  *out = endpoint;
  return SALTS_OK;
}

void flowie_control_auth_iris_endpoint_destroy(flowie_control_auth_iris_endpoint_t *endpoint) {
  if (!endpoint) return;
  if (endpoint->bound_app) {
    (void)flowie_control_http_app_unpost(endpoint->bound_app, FLOWIE_CONTROL_AUTH_HTTP_PATH,
                                         flowie_control_auth_iris_registered_handler);
    (void)flowie_control_http_app_unbind_context(endpoint->bound_app,
                                                 FLOWIE_CONTROL_AUTH_HTTP_PATH, endpoint);
  }
  salts_threadpool_destroy(endpoint->local_executor);
  endpoint->local_executor = NULL;
  crypto_wipe(endpoint, sizeof(*endpoint));
  free(endpoint);
}

int flowie_control_auth_iris_endpoint_register(flowie_control_auth_iris_endpoint_t *endpoint,
                                               flowie_control_http_app_t *app) {
  if (!endpoint || !app || endpoint->bound_app) return SALTS_EINVAL;
  if (flowie_control_http_app_bind_context(app, FLOWIE_CONTROL_AUTH_HTTP_PATH, endpoint) !=
      SALTS_OK)
    return SALTS_EBUSY;
  endpoint->bound_app = app;
  if (flowie_control_http_app_post(app, FLOWIE_CONTROL_AUTH_HTTP_PATH,
                                   flowie_control_auth_iris_registered_handler) != SALTS_OK) {
    endpoint->bound_app = NULL;
    (void)flowie_control_http_app_unbind_context(app, FLOWIE_CONTROL_AUTH_HTTP_PATH, endpoint);
    return SALTS_EBUSY;
  }
  return SALTS_OK;
}
