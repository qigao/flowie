#include "flowie_server_http_security_internal.h"

#include "base64_utils.h"
#include "CoroNet/turbo_coro_context.h"
#include "turbo_coro.h"
#include "turbo_error.h"
#include "turbo_http.h"
#include "turbo_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_SERVER_AUTH_PROTOCOL_VERSION = 3u,
  FLOWIE_SERVER_ACL_PROTOCOL_VERSION = 4u,
  FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT = 65536u
};

struct flowie_server_http_security_s {
  flowie_server_http_provider_config_t auth_config;
  flowie_server_http_provider_config_t acl_config;
  char *auth_token;
  char *acl_token;
  turbo_http_t *auth_client;
  turbo_http_t *acl_client;
  flowie_security_auth_provider_t auth_provider;
  flowie_security_authorization_provider_t acl_provider;
};

static void flowie_server_secure_clear(void *memory, size_t size) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (cursor && size-- != 0u)
    *cursor++ = 0u;
}

void flowie_server_http_body_destroy(char *body, size_t body_size, int sensitive) {
  if (!body) return;
  if (sensitive) flowie_server_secure_clear(body, body_size);
  turbo_json_serialize_free(body);
}

static void flowie_server_json_destroy(json_value_t *value) {
  turbo_json_doc_t *owned = (turbo_json_doc_t *)value;
  if (owned) turbo_free_json(&owned);
}

static int flowie_server_json_add(json_value_t *object, const char *key, json_value_t *value) {
  if (value && turbo_json_object_add_checked(object, key, value)) return TURBO_OK;
  flowie_server_json_destroy(value);
  return TURBO_ENOMEM;
}

static int flowie_server_json_array_add(json_value_t *array, json_value_t *value) {
  if (value && turbo_json_array_add_checked(array, value)) return TURBO_OK;
  flowie_server_json_destroy(value);
  return TURBO_ENOMEM;
}

static int flowie_server_json_u64(const json_value_t *value, uint64_t *out) {
  const char *text;
  char *end = NULL;
  size_t length = 0u;
  unsigned long long parsed;
  if (!value || !out || turbo_json_type(value) != TURBO_JSON_NUMBER) return TURBO_EPROTO;
  text = turbo_json_number_text(value, &length);
  if (!text || length == 0u || length >= 32u) return TURBO_EPROTO;
  {
    char copy[32];
    memcpy(copy, text, length);
    copy[length] = '\0';
    errno = 0;
    parsed = strtoull(copy, &end, 10);
    if (errno != 0 || end != copy + length) return TURBO_EPROTO;
  }
  *out = (uint64_t)parsed;
  return TURBO_OK;
}

static int flowie_server_json_copy(const json_value_t *object, const char *key, char *out,
                                   size_t capacity, int required) {
  json_value_t *value;
  const char *text;
  size_t length;
  if (!object || !key || !out || capacity == 0u) return TURBO_EINVAL;
  value = turbo_json_object_get(object, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_STRING) return TURBO_EPROTO;
  text = turbo_json_string(value);
  length = turbo_json_string_len(value);
  if (!text || (required && length == 0u) || length >= capacity) return TURBO_EPROTO;
  memcpy(out, text, length);
  out[length] = '\0';
  return TURBO_OK;
}

static int flowie_server_json_strings(const json_value_t *object, const char *key, char *out,
                                      size_t stride, uint32_t maximum, uint32_t *count_out) {
  json_value_t *array = turbo_json_object_get(object, key);
  size_t count;
  if (!array || turbo_json_type(array) != TURBO_JSON_ARRAY || !out || !count_out)
    return TURBO_EPROTO;
  count = turbo_json_array_size(array);
  if (count > maximum) return TURBO_EPROTO;
  for (size_t i = 0u; i < count; ++i) {
    json_value_t *entry = turbo_json_array_get(array, i);
    const char *text;
    size_t length;
    if (!entry || turbo_json_type(entry) != TURBO_JSON_STRING) return TURBO_EPROTO;
    text = turbo_json_string(entry);
    length = turbo_json_string_len(entry);
    if (!text || length == 0u || length >= stride) return TURBO_EPROTO;
    memcpy(out + i * stride, text, length);
    out[i * stride + length] = '\0';
  }
  *count_out = (uint32_t)count;
  return TURBO_OK;
}

int flowie_server_http_auth_encode(const flowie_security_auth_request_t *request,
                                   char **body_out, size_t *body_size_out) {
  turbo_json_doc_t *document = NULL;
  char *secret_base64 = NULL;
  const char *fingerprint = NULL;
  int rc = TURBO_ENOMEM;
  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!request || request->size < FLOWIE_SECURITY_AUTH_REQUEST_BASE_SIZE || !body_out ||
      !body_size_out || !request->identity || !request->identity[0] || !request->method ||
      !request->method[0] || !request->secret || request->secret_size == 0u)
    return TURBO_EINVAL;
  if (request->size >= sizeof(*request)) fingerprint = request->peer_certificate_sha256;
  if (tn_base64_encode(request->secret, request->secret_size, &secret_base64) != 0 ||
      !secret_base64)
    return TURBO_ENOMEM;
  document = (turbo_json_doc_t *)turbo_json_create_object();
  if (!document) goto done;
  if (flowie_server_json_add(document, "version",
                             turbo_json_create_uint64(FLOWIE_SERVER_AUTH_PROTOCOL_VERSION)) !=
          TURBO_OK ||
      flowie_server_json_add(document, "identity",
                             turbo_json_create_string(request->identity)) != TURBO_OK ||
      flowie_server_json_add(document, "method", turbo_json_create_string(request->method)) !=
          TURBO_OK ||
      flowie_server_json_add(document, "secret_base64",
                             turbo_json_create_string(secret_base64)) != TURBO_OK ||
      flowie_server_json_add(document, "protocol",
                             turbo_json_create_string(request->protocol ? request->protocol :
                                                                          "")) != TURBO_OK ||
      flowie_server_json_add(
          document, "remote_address",
          turbo_json_create_string(request->remote_address ? request->remote_address : "")) !=
          TURBO_OK ||
      flowie_server_json_add(
          document, "peer_certificate_sha256",
          turbo_json_create_string(fingerprint ? fingerprint : "")) != TURBO_OK)
    goto done;
  *body_out = turbo_json_serialize(document, body_size_out);
  if (!*body_out) goto done;
  rc = TURBO_OK;

done:
  if (secret_base64) {
    flowie_server_secure_clear(secret_base64, strlen(secret_base64));
    free(secret_base64);
  }
  turbo_free_json(&document);
  return rc;
}

static int flowie_server_scope(const char *text, flowie_security_scope_t *scope) {
  if (!text || !scope) return TURBO_EPROTO;
  if (strcmp(text, "self") == 0) *scope = FLOWIE_SECURITY_SCOPE_SELF;
  else if (strcmp(text, "group") == 0) *scope = FLOWIE_SECURITY_SCOPE_GROUP;
  else if (strcmp(text, "domain") == 0) *scope = FLOWIE_SECURITY_SCOPE_DOMAIN;
  else if (strcmp(text, "system") == 0) *scope = FLOWIE_SECURITY_SCOPE_SYSTEM;
  else return TURBO_EPROTO;
  return TURBO_OK;
}

int flowie_server_http_auth_decode(const char *body, size_t body_size, const char *method,
                                   flowie_security_principal_t *principal_out) {
  turbo_json_doc_t *document = NULL;
  json_value_t *authenticated;
  json_value_t *principal;
  json_value_t *scope;
  uint64_t version = 0u;
  int rc = TURBO_EPROTO;
  if (!body || body_size == 0u || body_size > FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT || !method ||
      !principal_out || principal_out->size < sizeof(*principal_out))
    return TURBO_EINVAL;
  *principal_out = (flowie_security_principal_t)FLOWIE_SECURITY_PRINCIPAL_INIT;
  if (turbo_parse_json((const uint8_t *)body, body_size, &document) != TURBO_OK || !document)
    return TURBO_EPROTO;
  if (flowie_server_json_u64(turbo_json_object_get(document, "version"), &version) != TURBO_OK ||
      version != FLOWIE_SERVER_AUTH_PROTOCOL_VERSION)
    goto done;
  authenticated = turbo_json_object_get(document, "authenticated");
  if (!authenticated || turbo_json_type(authenticated) != TURBO_JSON_BOOL) goto done;
  if (!turbo_json_bool(authenticated)) {
    rc = TURBO_EPERM;
    goto done;
  }
  principal = turbo_json_object_get(document, "principal");
  if (!principal || turbo_json_type(principal) != TURBO_JSON_OBJECT) goto done;
  if (flowie_server_json_copy(principal, "id", principal_out->principal_id,
                              sizeof(principal_out->principal_id), 1) != TURBO_OK ||
      flowie_server_json_copy(principal, "type", principal_out->principal_type,
                              sizeof(principal_out->principal_type), 1) != TURBO_OK ||
      flowie_server_json_copy(principal, "domain", principal_out->domain_id,
                              sizeof(principal_out->domain_id), 0) != TURBO_OK ||
      flowie_server_json_copy(principal, "auth_method", principal_out->auth_method,
                              sizeof(principal_out->auth_method), 1) != TURBO_OK ||
      strcmp(principal_out->auth_method, method) != 0)
    goto done;
  scope = turbo_json_object_get(principal, "scope");
  if (!scope || turbo_json_type(scope) != TURBO_JSON_STRING ||
      flowie_server_scope(turbo_json_string(scope), &principal_out->scope) != TURBO_OK ||
      flowie_server_json_strings(principal, "roles", (char *)principal_out->roles,
                                 sizeof(principal_out->roles[0]), FLOWIE_SECURITY_MAX_ROLES,
                                 &principal_out->role_count) != TURBO_OK ||
      flowie_server_json_strings(principal, "groups", (char *)principal_out->groups,
                                 sizeof(principal_out->groups[0]), FLOWIE_SECURITY_MAX_GROUPS,
                                 &principal_out->group_count) != TURBO_OK ||
      flowie_server_json_u64(turbo_json_object_get(principal, "expires_at"),
                             &principal_out->expires_at) != TURBO_OK ||
      flowie_server_json_u64(turbo_json_object_get(principal, "policy_version"),
                             &principal_out->policy_version) != TURBO_OK ||
      principal_out->policy_version == 0u ||
      (principal_out->scope != FLOWIE_SECURITY_SCOPE_SYSTEM && !principal_out->domain_id[0]))
    goto done;
  rc = TURBO_OK;

done:
  turbo_free_json(&document);
  if (rc != TURBO_OK)
    *principal_out = (flowie_security_principal_t)FLOWIE_SECURITY_PRINCIPAL_INIT;
  return rc;
}

static int flowie_server_json_add_strings(json_value_t *object, const char *key,
                                          const char *values, size_t stride, uint32_t count) {
  json_value_t *array = turbo_json_create_array();
  int rc = array ? TURBO_OK : TURBO_ENOMEM;
  for (uint32_t i = 0u; rc == TURBO_OK && i < count; ++i)
    rc = flowie_server_json_array_add(array,
                                      turbo_json_create_string(values + (size_t)i * stride));
  if (rc == TURBO_OK) {
    rc = flowie_server_json_add(object, key, array);
    array = NULL;
  }
  flowie_server_json_destroy(array);
  return rc;
}

static int flowie_server_span_copy(const uint8_t *data, size_t size, char **out) {
  char *copy;
  if (out) *out = NULL;
  if (!out || (size != 0u && !data) || size > FLOWIE_SECURITY_ID_MAX ||
      (data && memchr(data, '\0', size)))
    return TURBO_EINVAL;
  copy = (char *)malloc(size + 1u);
  if (!copy) return TURBO_ENOMEM;
  if (size != 0u) memcpy(copy, data, size);
  copy[size] = '\0';
  *out = copy;
  return TURBO_OK;
}

int flowie_server_http_acl_encode(const flowie_security_request_t *request,
                                  char **body_out, size_t *body_size_out) {
  turbo_json_doc_t *document = NULL;
  json_value_t *principal = NULL;
  char *username = NULL;
  char *client_id = NULL;
  const char *access;
  int rc;
  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!request || request->size < sizeof(*request) || !request->principal ||
      request->principal->size < sizeof(*request->principal) || !request->resource ||
      !body_out || !body_size_out ||
      request->principal->role_count > FLOWIE_SECURITY_MAX_ROLES ||
      request->principal->group_count > FLOWIE_SECURITY_MAX_GROUPS)
    return TURBO_EINVAL;
  access = request->action == FLOWIE_SECURITY_ACTION_SUBSCRIBE
               ? "read"
               : request->action == FLOWIE_SECURITY_ACTION_PUBLISH
                     ? "write"
                     : request->action == FLOWIE_SECURITY_ACTION_CONNECT ? "connect" : NULL;
  if (!access) return TURBO_EINVAL;
  rc = flowie_server_span_copy(request->username, request->username_size, &username);
  if (rc == TURBO_OK)
    rc = flowie_server_span_copy(request->client_id, request->client_id_size, &client_id);
  if (rc != TURBO_OK) goto done;
  document = (turbo_json_doc_t *)turbo_json_create_object();
  principal = turbo_json_create_object();
  if (!document || !principal) {
    rc = TURBO_ENOMEM;
    goto done;
  }
  if (flowie_server_json_add(principal, "id",
                             turbo_json_create_string(request->principal->principal_id)) !=
          TURBO_OK ||
      flowie_server_json_add(principal, "type",
                             turbo_json_create_string(request->principal->principal_type)) !=
          TURBO_OK ||
      flowie_server_json_add(principal, "domain",
                             turbo_json_create_string(request->principal->domain_id)) !=
          TURBO_OK ||
      flowie_server_json_add(principal, "expires_at",
                             turbo_json_create_uint64(request->principal->expires_at)) !=
          TURBO_OK ||
      flowie_server_json_add(principal, "policy_version",
                             turbo_json_create_uint64(request->principal->policy_version)) !=
          TURBO_OK ||
      flowie_server_json_add_strings(principal, "roles", (const char *)request->principal->roles,
                                     sizeof(request->principal->roles[0]),
                                     request->principal->role_count) != TURBO_OK ||
      flowie_server_json_add_strings(principal, "groups",
                                     (const char *)request->principal->groups,
                                     sizeof(request->principal->groups[0]),
                                     request->principal->group_count) != TURBO_OK ||
      flowie_server_json_add(document, "version",
                             turbo_json_create_uint64(FLOWIE_SERVER_ACL_PROTOCOL_VERSION)) !=
          TURBO_OK ||
      flowie_server_json_add(document, "access", turbo_json_create_string(access)) != TURBO_OK ||
      flowie_server_json_add(document, "topic", turbo_json_create_string(request->resource)) !=
          TURBO_OK ||
      flowie_server_json_add(document, "username", turbo_json_create_string(username)) !=
          TURBO_OK ||
      flowie_server_json_add(document, "client_id", turbo_json_create_string(client_id)) !=
          TURBO_OK)
    goto no_memory;
  rc = flowie_server_json_add(document, "principal", principal);
  principal = NULL;
  if (rc != TURBO_OK) goto done;
  *body_out = turbo_json_serialize(document, body_size_out);
  rc = *body_out ? TURBO_OK : TURBO_ENOMEM;
  goto done;

no_memory:
  rc = TURBO_ENOMEM;
done:
  flowie_server_json_destroy(principal);
  turbo_free_json(&document);
  free(username);
  free(client_id);
  return rc;
}

static int flowie_server_decision_reason(const char *text,
                                         flowie_security_decision_reason_t *reason) {
  if (!text || !reason) return TURBO_EPROTO;
  if (strcmp(text, "allow_rule") == 0) *reason = FLOWIE_SECURITY_REASON_ALLOW_RULE;
  else if (strcmp(text, "deny_rule") == 0) *reason = FLOWIE_SECURITY_REASON_DENY_RULE;
  else if (strcmp(text, "default_deny") == 0) *reason = FLOWIE_SECURITY_REASON_DEFAULT_DENY;
  else if (strcmp(text, "domain_mismatch") == 0)
    *reason = FLOWIE_SECURITY_REASON_DOMAIN_MISMATCH;
  else if (strcmp(text, "principal_expired") == 0)
    *reason = FLOWIE_SECURITY_REASON_PRINCIPAL_EXPIRED;
  else if (strcmp(text, "policy_version_mismatch") == 0)
    *reason = FLOWIE_SECURITY_REASON_POLICY_VERSION_MISMATCH;
  else return TURBO_EPROTO;
  return TURBO_OK;
}

int flowie_server_http_acl_decode(const char *body, size_t body_size,
                                  flowie_security_decision_t *decision_out) {
  turbo_json_doc_t *document = NULL;
  json_value_t *allowed;
  json_value_t *reason;
  uint64_t version = 0u;
  int rc = TURBO_EPROTO;
  if (!body || body_size == 0u || body_size > FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT ||
      !decision_out || decision_out->size < sizeof(*decision_out))
    return TURBO_EINVAL;
  *decision_out = (flowie_security_decision_t)FLOWIE_SECURITY_DECISION_INIT;
  if (turbo_parse_json((const uint8_t *)body, body_size, &document) != TURBO_OK || !document)
    return TURBO_EPROTO;
  allowed = turbo_json_object_get(document, "allowed");
  reason = turbo_json_object_get(document, "reason");
  if (flowie_server_json_u64(turbo_json_object_get(document, "version"), &version) != TURBO_OK ||
      version != FLOWIE_SERVER_ACL_PROTOCOL_VERSION || !allowed ||
      turbo_json_type(allowed) != TURBO_JSON_BOOL || !reason ||
      turbo_json_type(reason) != TURBO_JSON_STRING ||
      flowie_server_decision_reason(turbo_json_string(reason), &decision_out->reason) != TURBO_OK ||
      flowie_server_json_u64(turbo_json_object_get(document, "policy_version"),
                             &decision_out->policy_version) != TURBO_OK ||
      decision_out->policy_version == 0u)
    goto done;
  decision_out->effect =
      turbo_json_bool(allowed) ? FLOWIE_SECURITY_ALLOW : FLOWIE_SECURITY_DENY;
  if ((decision_out->effect == FLOWIE_SECURITY_ALLOW &&
       decision_out->reason != FLOWIE_SECURITY_REASON_ALLOW_RULE) ||
      (decision_out->effect == FLOWIE_SECURITY_DENY &&
       decision_out->reason == FLOWIE_SECURITY_REASON_ALLOW_RULE))
    goto done;
  rc = TURBO_OK;

done:
  turbo_free_json(&document);
  if (rc != TURBO_OK)
    *decision_out = (flowie_security_decision_t)FLOWIE_SECURITY_DECISION_INIT;
  return rc;
}

static int flowie_server_http_config_valid(const flowie_server_http_provider_config_t *config,
                                           int auth) {
  return config && strncmp(config->url, "https://", 8u) == 0 && config->service_id[0] &&
         config->service_domain[0] && strncmp(config->service_token_ref, "env://", 6u) == 0 &&
         config->service_token_ref[6] && config->ca_file[0] && config->timeout_ms > 0u &&
         config->max_body_size > 0u && (!auth || config->method[0]);
}

static int flowie_server_http_token_copy(const char *reference, char **out) {
  const char *value;
  size_t size;
  char *copy;
  if (out) *out = NULL;
  if (!reference || strncmp(reference, "env://", 6u) != 0 || !reference[6] || !out)
    return TURBO_EINVAL;
  value = getenv(reference + 6u);
  if (!value || !value[0]) return TURBO_ENOENT;
  size = strlen(value);
  if (size > 4096u || memchr(value, '\r', size) || memchr(value, '\n', size)) return TURBO_EPERM;
  copy = (char *)malloc(size + 1u);
  if (!copy) return TURBO_ENOMEM;
  memcpy(copy, value, size + 1u);
  *out = copy;
  return TURBO_OK;
}

static void flowie_server_http_token_destroy(char *token) {
  if (!token) return;
  flowie_server_secure_clear(token, strlen(token));
  free(token);
}

static int flowie_server_http_client_create(coro_context_t *context,
                                            const flowie_server_http_provider_config_t *config,
                                            const char *token, size_t max_response_size,
                                            turbo_http_t **out) {
  turbo_http_options_t options;
  turbo_tls_client_config_t tls = {0};
  int rc;
  if (out) *out = NULL;
  if (!context || !config || !token || !out || max_response_size == 0u) return TURBO_EINVAL;
  rc = turbo_http_options_init(&options, sizeof(options));
  if (rc != TURBO_OK) return rc;
  options.follow_redirects = 0;
  options.max_redirects = 0;
  options.timeout_ms = (int64_t)config->timeout_ms;
  memset(&options.retry, 0, sizeof(options.retry));
  rc = turbo_http_create(context, &options, out);
  if (rc != TURBO_OK) return rc;
  tls.verify_peer = 1;
  tls.ca_file = config->ca_file;
  rc = turbo_http_set_tls_config(*out, &tls);
  if (rc == TURBO_OK) rc = turbo_http_set_bearer_token(*out, token);
  if (rc != TURBO_OK) {
    turbo_http_destroy(*out);
    *out = NULL;
    return rc;
  }
  turbo_http_set_max_response_size(*out, max_response_size);
  turbo_http_set_max_response_header_size(*out, 16384u);
  return TURBO_OK;
}

static int flowie_server_http_client_on_current_context(turbo_http_t *client) {
  return client && coro_running() && coro_context_current() == turbo_http_get_context(client);
}

static int flowie_server_http_response_status(http_response_t *response) {
  if (!response || response->error_code != HTTP_ERROR_NONE) return TURBO_EIO;
  if (response->status_code == 401 || response->status_code == 403) return TURBO_EPERM;
  if (response->status_code == 429) return TURBO_EBUSY;
  if (response->status_code != 200 || !http_response_is_json(response)) return TURBO_EIO;
  return TURBO_OK;
}

int flowie_server_http_headers(const flowie_server_http_provider_config_t *config,
                               char *service_id, size_t service_id_size,
                               char *service_domain, size_t service_domain_size,
                               const char *headers[4]) {
  int id_length;
  int domain_length;
  if (!config || !service_id || !service_domain || !headers) return TURBO_EINVAL;
  id_length =
      snprintf(service_id, service_id_size, "X-Flowie-Service-Id: %s", config->service_id);
  domain_length = snprintf(service_domain, service_domain_size,
                           "X-Flowie-Service-Domain: %s", config->service_domain);
  if (id_length <= 0 || (size_t)id_length >= service_id_size || domain_length <= 0 ||
      (size_t)domain_length >= service_domain_size)
    return TURBO_ERANGE;
  headers[0] = "Content-Type: application/json";
  headers[1] = "Accept: application/json";
  headers[2] = service_id;
  headers[3] = service_domain;
  return TURBO_OK;
}

static int flowie_server_http_authenticate(void *ctx,
                                           const flowie_security_auth_request_t *request,
                                           flowie_security_principal_t *principal_out) {
  flowie_server_http_security_t *security = (flowie_server_http_security_t *)ctx;
  turbo_http_t *client = NULL;
  http_response_t *response = NULL;
  const char *headers[4];
  char service_id[sizeof("X-Flowie-Service-Id: ") + FLOWIE_SECURITY_ID_MAX + 1u];
  char service_domain[sizeof("X-Flowie-Service-Domain: ") + FLOWIE_SECURITY_ID_MAX + 1u];
  char *body = NULL;
  size_t body_size = 0u;
  int rc;
  if (!security || !request || !principal_out || request->secret_size == 0u ||
      request->secret_size > security->auth_config.max_body_size ||
      strcmp(request->method, security->auth_config.method) != 0)
    return TURBO_EPERM;
  client = security->auth_client;
  if (!flowie_server_http_client_on_current_context(client)) return TURBO_ENOTSUP;
  rc = flowie_server_http_auth_encode(request, &body, &body_size);
  if (rc != TURBO_OK) return rc;
  rc = flowie_server_http_headers(&security->auth_config, service_id, sizeof(service_id),
                                  service_domain, sizeof(service_domain), headers);
  if (rc == TURBO_OK)
    response = turbo_http_request(client, HTTP_POST, security->auth_config.url, headers, 4, body,
                                  body_size);
  if (rc == TURBO_OK) rc = flowie_server_http_response_status(response);
  if (rc == TURBO_OK)
    rc = flowie_server_http_auth_decode(response->body, response->body_len,
                                        security->auth_config.method, principal_out);
  http_response_free(response);
  flowie_server_http_body_destroy(body, body_size, 1);
  return rc;
}

static int flowie_server_http_authorize(void *ctx, const flowie_security_request_t *request,
                                        uint64_t now_epoch_seconds,
                                        flowie_security_decision_t *decision_out) {
  flowie_server_http_security_t *security = (flowie_server_http_security_t *)ctx;
  turbo_http_t *client = NULL;
  http_response_t *response = NULL;
  const char *headers[4];
  char service_id[sizeof("X-Flowie-Service-Id: ") + FLOWIE_SECURITY_ID_MAX + 1u];
  char service_domain[sizeof("X-Flowie-Service-Domain: ") + FLOWIE_SECURITY_ID_MAX + 1u];
  char *body = NULL;
  size_t body_size = 0u;
  int rc;
  (void)now_epoch_seconds;
  if (!security || !request || !decision_out) return TURBO_EINVAL;
  client = security->acl_client;
  if (!flowie_server_http_client_on_current_context(client)) return TURBO_ENOTSUP;
  rc = flowie_server_http_acl_encode(request, &body, &body_size);
  if (rc != TURBO_OK) return rc;
  rc = flowie_server_http_headers(&security->acl_config, service_id, sizeof(service_id),
                                  service_domain, sizeof(service_domain), headers);
  if (rc == TURBO_OK)
    response = turbo_http_request(client, HTTP_POST, security->acl_config.url, headers, 4, body,
                                  body_size);
  if (rc == TURBO_OK) rc = flowie_server_http_response_status(response);
  if (rc == TURBO_OK)
    rc = flowie_server_http_acl_decode(response->body, response->body_len, decision_out);
  http_response_free(response);
  flowie_server_http_body_destroy(body, body_size, 0);
  return rc;
}

int flowie_server_http_security_create(coro_context_t *context,
                                       const flowie_server_http_provider_config_t *auth,
                                       const flowie_server_http_provider_config_t *acl,
                                       flowie_server_http_security_t **out) {
  flowie_server_http_security_t *security;
  int rc;
  if (out) *out = NULL;
  if (!context || !flowie_server_http_config_valid(auth, 1) ||
      !flowie_server_http_config_valid(acl, 0) || !out)
    return TURBO_EINVAL;
  security = (flowie_server_http_security_t *)calloc(1u, sizeof(*security));
  if (!security) return TURBO_ENOMEM;
  security->auth_config = *auth;
  security->acl_config = *acl;
  rc = flowie_server_http_token_copy(auth->service_token_ref, &security->auth_token);
  if (rc == TURBO_OK)
    rc = flowie_server_http_token_copy(acl->service_token_ref, &security->acl_token);
  if (rc == TURBO_OK)
    rc = flowie_server_http_client_create(context, &security->auth_config, security->auth_token,
                                          FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT,
                                          &security->auth_client);
  if (rc == TURBO_OK)
    rc = flowie_server_http_client_create(context, &security->acl_config, security->acl_token,
                                          security->acl_config.max_body_size,
                                          &security->acl_client);
  if (rc != TURBO_OK) {
    flowie_server_http_security_destroy(security);
    return rc;
  }
  security->auth_provider = (flowie_security_auth_provider_t){
      sizeof(security->auth_provider), security, flowie_server_http_authenticate};
  security->acl_provider = (flowie_security_authorization_provider_t){
      sizeof(security->acl_provider), security, flowie_server_http_authorize};
  *out = security;
  return TURBO_OK;
}

void flowie_server_http_security_destroy(flowie_server_http_security_t *security) {
  if (!security) return;
  turbo_http_destroy(security->acl_client);
  turbo_http_destroy(security->auth_client);
  flowie_server_http_token_destroy(security->acl_token);
  flowie_server_http_token_destroy(security->auth_token);
  flowie_server_secure_clear(security, sizeof(*security));
  free(security);
}

const flowie_security_auth_provider_t *flowie_server_http_security_auth_provider(
    const flowie_server_http_security_t *security) {
  return security ? &security->auth_provider : NULL;
}

const flowie_security_authorization_provider_t *flowie_server_http_security_acl_provider(
    const flowie_server_http_security_t *security) {
  return security ? &security->acl_provider : NULL;
}
