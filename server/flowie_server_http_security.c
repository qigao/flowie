#include "flowie_server_http_security_internal.h"

#include "base64_utils.h"
#include <chttp/chttp.h>
#include "salts_error.h"
#include <json_parser.h>
#include <uri_parser.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_SERVER_AUTH_PROTOCOL_VERSION = 3u,
  FLOWIE_SERVER_ACL_PROTOCOL_VERSION = 4u,
  FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT = 65536u,
  FLOWIE_SERVER_HTTP_HEADER_LIMIT = 16384u,
  FLOWIE_SERVER_HTTP_DESTROY_TIMEOUT_MS = 5000u
};

typedef struct flowie_server_http_endpoint_s {
  char connection_uri[320];
  char authority[320];
  char target[2048];
  char server_name[256];
  char ca_file[1025];
  size_t max_request_size;
  size_t max_response_size;
  uint32_t timeout_ms;
} flowie_server_http_endpoint_t;

struct flowie_server_http_security_s {
  flowie_server_http_provider_config_t auth_config;
  flowie_server_http_provider_config_t acl_config;
  char *auth_token;
  char *acl_token;
  flowie_server_http_endpoint_t auth_endpoint;
  flowie_server_http_endpoint_t acl_endpoint;
  flowie_security_auth_provider_t auth_provider;
  flowie_security_authorization_provider_t acl_provider;
};

static native_io_backend_kind flowie_server_http_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void flowie_server_secure_clear(void *memory, size_t size) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (cursor && size-- != 0u)
    *cursor++ = 0u;
}

void flowie_server_http_body_destroy(char *body, size_t body_size, int sensitive) {
  if (!body) return;
  if (sensitive) flowie_server_secure_clear(body, body_size);
  json_serialize_free(body);
}

static void flowie_server_json_destroy(json_value_t *value) {
  json_value_t *owned = (json_value_t *)value;
  if (owned) json_free(owned);
}

static int flowie_server_json_add(json_value_t *object, const char *key, json_value_t *value) {
  if (value && json_object_add_checked(object, key, value)) return SALTS_OK;
  flowie_server_json_destroy(value);
  return SALTS_ENOMEM;
}

static int flowie_server_json_array_add(json_value_t *array, json_value_t *value) {
  if (value && json_array_add_checked(array, value)) return SALTS_OK;
  flowie_server_json_destroy(value);
  return SALTS_ENOMEM;
}

static int flowie_server_json_u64(const json_value_t *value, uint64_t *out) {
  const char *text;
  char *end = NULL;
  size_t length = 0u;
  unsigned long long parsed;
  if (!value || !out || json_type(value) != JSON_NUMBER) return SALTS_EPROTO;
  text = json_number_text(value, &length);
  if (!text || length == 0u || length >= 32u) return SALTS_EPROTO;
  {
    char copy[32];
    memcpy(copy, text, length);
    copy[length] = '\0';
    errno = 0;
    parsed = strtoull(copy, &end, 10);
    if (errno != 0 || end != copy + length) return SALTS_EPROTO;
  }
  *out = (uint64_t)parsed;
  return SALTS_OK;
}

static int flowie_server_json_copy(const json_value_t *object, const char *key, char *out,
                                   size_t capacity, int required) {
  json_value_t *value;
  const char *text;
  size_t length;
  if (!object || !key || !out || capacity == 0u) return SALTS_EINVAL;
  value = json_object_get(object, key);
  if (!value || json_type(value) != JSON_STRING) return SALTS_EPROTO;
  text = json_string(value);
  length = json_string_len(value);
  if (!text || (required && length == 0u) || length >= capacity) return SALTS_EPROTO;
  memcpy(out, text, length);
  out[length] = '\0';
  return SALTS_OK;
}

static int flowie_server_json_strings(const json_value_t *object, const char *key, char *out,
                                      size_t stride, uint32_t maximum, uint32_t *count_out) {
  json_value_t *array = json_object_get(object, key);
  size_t count;
  if (!array || json_type(array) != JSON_ARRAY || !out || !count_out)
    return SALTS_EPROTO;
  count = json_array_size(array);
  if (count > maximum) return SALTS_EPROTO;
  for (size_t i = 0u; i < count; ++i) {
    json_value_t *entry = json_array_get(array, i);
    const char *text;
    size_t length;
    if (!entry || json_type(entry) != JSON_STRING) return SALTS_EPROTO;
    text = json_string(entry);
    length = json_string_len(entry);
    if (!text || length == 0u || length >= stride) return SALTS_EPROTO;
    memcpy(out + i * stride, text, length);
    out[i * stride + length] = '\0';
  }
  *count_out = (uint32_t)count;
  return SALTS_OK;
}

int flowie_server_http_auth_encode(const flowie_security_auth_request_t *request,
                                   char **body_out, size_t *body_size_out) {
  json_value_t *document = NULL;
  char *secret_base64 = NULL;
  const char *fingerprint = NULL;
  int rc = SALTS_ENOMEM;
  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!request || request->size < FLOWIE_SECURITY_AUTH_REQUEST_BASE_SIZE || !body_out ||
      !body_size_out || !request->identity || !request->identity[0] || !request->method ||
      !request->method[0] || !request->secret || request->secret_size == 0u)
    return SALTS_EINVAL;
  if (request->size >= sizeof(*request)) fingerprint = request->peer_certificate_sha256;
  if (tn_base64_encode(request->secret, request->secret_size, &secret_base64) != 0 ||
      !secret_base64)
    return SALTS_ENOMEM;
  document = (json_value_t *)json_create_object();
  if (!document) goto done;
  if (flowie_server_json_add(document, "version",
                             json_create_uint64(FLOWIE_SERVER_AUTH_PROTOCOL_VERSION)) !=
          SALTS_OK ||
      flowie_server_json_add(document, "identity",
                             json_create_string(request->identity)) != SALTS_OK ||
      flowie_server_json_add(document, "method", json_create_string(request->method)) !=
          SALTS_OK ||
      flowie_server_json_add(document, "secret_base64",
                             json_create_string(secret_base64)) != SALTS_OK ||
      flowie_server_json_add(document, "protocol",
                             json_create_string(request->protocol ? request->protocol :
                                                                          "")) != SALTS_OK ||
      flowie_server_json_add(
          document, "remote_address",
          json_create_string(request->remote_address ? request->remote_address : "")) !=
          SALTS_OK ||
      flowie_server_json_add(
          document, "peer_certificate_sha256",
          json_create_string(fingerprint ? fingerprint : "")) != SALTS_OK)
    goto done;
  *body_out = json_serialize(document, body_size_out);
  if (!*body_out) goto done;
  rc = SALTS_OK;

done:
  if (secret_base64) {
    flowie_server_secure_clear(secret_base64, strlen(secret_base64));
    free(secret_base64);
  }
  json_free(document);
  return rc;
}

static int flowie_server_scope(const char *text, flowie_security_scope_t *scope) {
  if (!text || !scope) return SALTS_EPROTO;
  if (strcmp(text, "self") == 0) *scope = FLOWIE_SECURITY_SCOPE_SELF;
  else if (strcmp(text, "group") == 0) *scope = FLOWIE_SECURITY_SCOPE_GROUP;
  else if (strcmp(text, "domain") == 0) *scope = FLOWIE_SECURITY_SCOPE_DOMAIN;
  else if (strcmp(text, "system") == 0) *scope = FLOWIE_SECURITY_SCOPE_SYSTEM;
  else return SALTS_EPROTO;
  return SALTS_OK;
}

int flowie_server_http_auth_decode(const char *body, size_t body_size, const char *method,
                                   flowie_security_principal_t *principal_out) {
  json_value_t *document = NULL;
  json_value_t *authenticated;
  json_value_t *principal;
  json_value_t *scope;
  uint64_t version = 0u;
  int rc = SALTS_EPROTO;
  if (!body || body_size == 0u || body_size > FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT || !method ||
      !principal_out || principal_out->size < sizeof(*principal_out))
    return SALTS_EINVAL;
  *principal_out = (flowie_security_principal_t)FLOWIE_SECURITY_PRINCIPAL_INIT;
  document = json_parse(body, body_size);
  if (!document)
    return SALTS_EPROTO;
  if (flowie_server_json_u64(json_object_get(document, "version"), &version) != SALTS_OK ||
      version != FLOWIE_SERVER_AUTH_PROTOCOL_VERSION)
    goto done;
  authenticated = json_object_get(document, "authenticated");
  if (!authenticated || json_type(authenticated) != JSON_BOOL) goto done;
  if (!json_bool(authenticated)) {
    rc = SALTS_EPERM;
    goto done;
  }
  principal = json_object_get(document, "principal");
  if (!principal || json_type(principal) != JSON_OBJECT) goto done;
  if (flowie_server_json_copy(principal, "id", principal_out->principal_id,
                              sizeof(principal_out->principal_id), 1) != SALTS_OK ||
      flowie_server_json_copy(principal, "type", principal_out->principal_type,
                              sizeof(principal_out->principal_type), 1) != SALTS_OK ||
      flowie_server_json_copy(principal, "domain", principal_out->domain_id,
                              sizeof(principal_out->domain_id), 0) != SALTS_OK ||
      flowie_server_json_copy(principal, "auth_method", principal_out->auth_method,
                              sizeof(principal_out->auth_method), 1) != SALTS_OK ||
      strcmp(principal_out->auth_method, method) != 0)
    goto done;
  scope = json_object_get(principal, "scope");
  if (!scope || json_type(scope) != JSON_STRING ||
      flowie_server_scope(json_string(scope), &principal_out->scope) != SALTS_OK ||
      flowie_server_json_strings(principal, "roles", (char *)principal_out->roles,
                                 sizeof(principal_out->roles[0]), FLOWIE_SECURITY_MAX_ROLES,
                                 &principal_out->role_count) != SALTS_OK ||
      flowie_server_json_strings(principal, "groups", (char *)principal_out->groups,
                                 sizeof(principal_out->groups[0]), FLOWIE_SECURITY_MAX_GROUPS,
                                 &principal_out->group_count) != SALTS_OK ||
      flowie_server_json_u64(json_object_get(principal, "expires_at"),
                             &principal_out->expires_at) != SALTS_OK ||
      flowie_server_json_u64(json_object_get(principal, "policy_version"),
                             &principal_out->policy_version) != SALTS_OK ||
      principal_out->policy_version == 0u ||
      (principal_out->scope != FLOWIE_SECURITY_SCOPE_SYSTEM && !principal_out->domain_id[0]))
    goto done;
  rc = SALTS_OK;

done:
  json_free(document);
  if (rc != SALTS_OK)
    *principal_out = (flowie_security_principal_t)FLOWIE_SECURITY_PRINCIPAL_INIT;
  return rc;
}

static int flowie_server_json_add_strings(json_value_t *object, const char *key,
                                          const char *values, size_t stride, uint32_t count) {
  json_value_t *array = json_create_array();
  int rc = array ? SALTS_OK : SALTS_ENOMEM;
  for (uint32_t i = 0u; rc == SALTS_OK && i < count; ++i)
    rc = flowie_server_json_array_add(array,
                                      json_create_string(values + (size_t)i * stride));
  if (rc == SALTS_OK) {
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
    return SALTS_EINVAL;
  copy = (char *)malloc(size + 1u);
  if (!copy) return SALTS_ENOMEM;
  if (size != 0u) memcpy(copy, data, size);
  copy[size] = '\0';
  *out = copy;
  return SALTS_OK;
}

int flowie_server_http_acl_encode(const flowie_security_request_t *request,
                                  char **body_out, size_t *body_size_out) {
  json_value_t *document = NULL;
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
    return SALTS_EINVAL;
  access = request->action == FLOWIE_SECURITY_ACTION_SUBSCRIBE
               ? "read"
               : request->action == FLOWIE_SECURITY_ACTION_PUBLISH
                     ? "write"
                     : request->action == FLOWIE_SECURITY_ACTION_CONNECT ? "connect" : NULL;
  if (!access) return SALTS_EINVAL;
  rc = flowie_server_span_copy(request->username, request->username_size, &username);
  if (rc == SALTS_OK)
    rc = flowie_server_span_copy(request->client_id, request->client_id_size, &client_id);
  if (rc != SALTS_OK) goto done;
  document = (json_value_t *)json_create_object();
  principal = json_create_object();
  if (!document || !principal) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  if (flowie_server_json_add(principal, "id",
                             json_create_string(request->principal->principal_id)) !=
          SALTS_OK ||
      flowie_server_json_add(principal, "type",
                             json_create_string(request->principal->principal_type)) !=
          SALTS_OK ||
      flowie_server_json_add(principal, "domain",
                             json_create_string(request->principal->domain_id)) !=
          SALTS_OK ||
      flowie_server_json_add(principal, "expires_at",
                             json_create_uint64(request->principal->expires_at)) !=
          SALTS_OK ||
      flowie_server_json_add(principal, "policy_version",
                             json_create_uint64(request->principal->policy_version)) !=
          SALTS_OK ||
      flowie_server_json_add_strings(principal, "roles", (const char *)request->principal->roles,
                                     sizeof(request->principal->roles[0]),
                                     request->principal->role_count) != SALTS_OK ||
      flowie_server_json_add_strings(principal, "groups",
                                     (const char *)request->principal->groups,
                                     sizeof(request->principal->groups[0]),
                                     request->principal->group_count) != SALTS_OK ||
      flowie_server_json_add(document, "version",
                             json_create_uint64(FLOWIE_SERVER_ACL_PROTOCOL_VERSION)) !=
          SALTS_OK ||
      flowie_server_json_add(document, "access", json_create_string(access)) != SALTS_OK ||
      flowie_server_json_add(document, "topic", json_create_string(request->resource)) !=
          SALTS_OK ||
      flowie_server_json_add(document, "username", json_create_string(username)) !=
          SALTS_OK ||
      flowie_server_json_add(document, "client_id", json_create_string(client_id)) !=
          SALTS_OK)
    goto no_memory;
  rc = flowie_server_json_add(document, "principal", principal);
  principal = NULL;
  if (rc != SALTS_OK) goto done;
  *body_out = json_serialize(document, body_size_out);
  rc = *body_out ? SALTS_OK : SALTS_ENOMEM;
  goto done;

no_memory:
  rc = SALTS_ENOMEM;
done:
  flowie_server_json_destroy(principal);
  json_free(document);
  free(username);
  free(client_id);
  return rc;
}

static int flowie_server_decision_reason(const char *text,
                                         flowie_security_decision_reason_t *reason) {
  if (!text || !reason) return SALTS_EPROTO;
  if (strcmp(text, "allow_rule") == 0) *reason = FLOWIE_SECURITY_REASON_ALLOW_RULE;
  else if (strcmp(text, "deny_rule") == 0) *reason = FLOWIE_SECURITY_REASON_DENY_RULE;
  else if (strcmp(text, "default_deny") == 0) *reason = FLOWIE_SECURITY_REASON_DEFAULT_DENY;
  else if (strcmp(text, "domain_mismatch") == 0)
    *reason = FLOWIE_SECURITY_REASON_DOMAIN_MISMATCH;
  else if (strcmp(text, "principal_expired") == 0)
    *reason = FLOWIE_SECURITY_REASON_PRINCIPAL_EXPIRED;
  else if (strcmp(text, "policy_version_mismatch") == 0)
    *reason = FLOWIE_SECURITY_REASON_POLICY_VERSION_MISMATCH;
  else return SALTS_EPROTO;
  return SALTS_OK;
}

int flowie_server_http_acl_decode(const char *body, size_t body_size,
                                  flowie_security_decision_t *decision_out) {
  json_value_t *document = NULL;
  json_value_t *allowed;
  json_value_t *reason;
  uint64_t version = 0u;
  int rc = SALTS_EPROTO;
  if (!body || body_size == 0u || body_size > FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT ||
      !decision_out || decision_out->size < sizeof(*decision_out))
    return SALTS_EINVAL;
  *decision_out = (flowie_security_decision_t)FLOWIE_SECURITY_DECISION_INIT;
  document = json_parse(body, body_size);
  if (!document)
    return SALTS_EPROTO;
  allowed = json_object_get(document, "allowed");
  reason = json_object_get(document, "reason");
  if (flowie_server_json_u64(json_object_get(document, "version"), &version) != SALTS_OK ||
      version != FLOWIE_SERVER_ACL_PROTOCOL_VERSION || !allowed ||
      json_type(allowed) != JSON_BOOL || !reason ||
      json_type(reason) != JSON_STRING ||
      flowie_server_decision_reason(json_string(reason), &decision_out->reason) != SALTS_OK ||
      flowie_server_json_u64(json_object_get(document, "policy_version"),
                             &decision_out->policy_version) != SALTS_OK ||
      decision_out->policy_version == 0u)
    goto done;
  decision_out->effect =
      json_bool(allowed) ? FLOWIE_SECURITY_ALLOW : FLOWIE_SECURITY_DENY;
  if ((decision_out->effect == FLOWIE_SECURITY_ALLOW &&
       decision_out->reason != FLOWIE_SECURITY_REASON_ALLOW_RULE) ||
      (decision_out->effect == FLOWIE_SECURITY_DENY &&
       decision_out->reason == FLOWIE_SECURITY_REASON_ALLOW_RULE))
    goto done;
  rc = SALTS_OK;

done:
  json_free(document);
  if (rc != SALTS_OK)
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
    return SALTS_EINVAL;
  value = getenv(reference + 6u);
  if (!value || !value[0]) return SALTS_ENOENT;
  size = strlen(value);
  if (size > 4096u || memchr(value, '\r', size) || memchr(value, '\n', size)) return SALTS_EPERM;
  copy = (char *)malloc(size + 1u);
  if (!copy) return SALTS_ENOMEM;
  memcpy(copy, value, size + 1u);
  *out = copy;
  return SALTS_OK;
}

static void flowie_server_http_token_destroy(char *token) {
  if (!token) return;
  flowie_server_secure_clear(token, strlen(token));
  free(token);
}

static int flowie_server_http_endpoint_init(
    flowie_server_http_endpoint_t *endpoint,
    const flowie_server_http_provider_config_t *config, size_t max_response_size) {
  uri_t uri = {0};
  int port;
  int written;
  if (!endpoint || !config || max_response_size == 0u || !uri_parse(config->url, &uri) ||
      !uri.valid || strcmp(uri.scheme, "https") != 0 || !uri.host[0] || uri.userinfo[0] ||
      uri.fragment[0] || uri.path[0] != '/')
    return SALTS_EINVAL;
  port = uri.port == 0 ? 443 : uri.port;
  if (port <= 0 || port > UINT16_MAX) return SALTS_EINVAL;
  if (uri.host_type == URI_HOST_IPV6ADDR) {
    written = snprintf(endpoint->connection_uri, sizeof(endpoint->connection_uri),
                       "tls://[%s]:%d", uri.host, port);
    if (written <= 0 || (size_t)written >= sizeof(endpoint->connection_uri)) return SALTS_ERANGE;
    written = snprintf(endpoint->authority, sizeof(endpoint->authority),
                       port == 443 ? "[%s]" : "[%s]:%d", uri.host, port);
  } else {
    written = snprintf(endpoint->connection_uri, sizeof(endpoint->connection_uri),
                       "tls://%s:%d", uri.host, port);
    if (written <= 0 || (size_t)written >= sizeof(endpoint->connection_uri)) return SALTS_ERANGE;
    written = snprintf(endpoint->authority, sizeof(endpoint->authority),
                       port == 443 ? "%s" : "%s:%d", uri.host, port);
  }
  if (written <= 0 || (size_t)written >= sizeof(endpoint->authority)) return SALTS_ERANGE;
  written = snprintf(endpoint->target, sizeof(endpoint->target), uri.query[0] ? "%s?%s" : "%s",
                     uri.path, uri.query);
  if (written <= 0 || (size_t)written >= sizeof(endpoint->target)) return SALTS_ERANGE;
  written = snprintf(endpoint->server_name, sizeof(endpoint->server_name), "%s", uri.host);
  if (written <= 0 || (size_t)written >= sizeof(endpoint->server_name)) return SALTS_ERANGE;
  endpoint->max_request_size = config->max_body_size;
  endpoint->max_response_size = max_response_size;
  endpoint->timeout_ms = config->timeout_ms;
  written = snprintf(endpoint->ca_file, sizeof(endpoint->ca_file), "%s", config->ca_file);
  return written > 0 && (size_t)written < sizeof(endpoint->ca_file) ? SALTS_OK : SALTS_ERANGE;
}

static chttp_client_config
flowie_server_http_client_config(const flowie_server_http_endpoint_t *endpoint) {
  size_t max_send = endpoint->max_request_size + FLOWIE_SERVER_HTTP_HEADER_LIMIT + 4096u;
  return (chttp_client_config){
      .network = {.backend = flowie_server_http_backend(),
                  .connection_capacity = 1u,
                  .command_capacity = 8u,
                  .request_capacity = 4u,
                  .completion_batch_capacity = 4u,
                  .event_capacity = 8u,
                  .max_send_bytes = max_send,
                  .receive_buffer_bytes = FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT,
                  .connect_timeout_ms = endpoint->timeout_ms,
                  .read_timeout_ms = endpoint->timeout_ms,
                  .write_timeout_ms = endpoint->timeout_ms,
                  .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                  .tls_handshake_timeout_ms = endpoint->timeout_ms},
      .request_capacity = 1u,
      .max_start_line_bytes = 2048u,
      .max_header_count = 16u,
      .max_header_bytes = FLOWIE_SERVER_HTTP_HEADER_LIMIT,
      .max_request_body_bytes = endpoint->max_request_size,
      .max_response_body_bytes = endpoint->max_response_size,
      .max_informational_responses = 4u};
}

static int flowie_server_http_post(const flowie_server_http_endpoint_t *endpoint,
                                   const flowie_server_http_provider_config_t *config,
                                   const char *token, const void *body, size_t body_size,
                                   chttp_response *response) {
  chttp_client client = {0};
  chttp_tls_profile tls_profile = {0};
  cnet_tls_client_config tls_config;
  chttp_error error = {0};
  chttp_client_config client_config;
  chttp_options options;
  chttp_header headers[5];
  char authorization[sizeof("Bearer ") + 4096u];
  int written;
  int rc;
  if (!endpoint || !config || !token || !body || body_size == 0u || !response)
    return SALTS_EINVAL;
  written = snprintf(authorization, sizeof(authorization), "Bearer %s", token);
  if (written <= 0 || (size_t)written >= sizeof(authorization)) return SALTS_ERANGE;
  headers[0] = (chttp_header){"Content-Type", "application/json"};
  headers[1] = (chttp_header){"Accept", "application/json"};
  headers[2] = (chttp_header){"X-Flowie-Service-Id", config->service_id};
  headers[3] = (chttp_header){"X-Flowie-Service-Domain", config->service_domain};
  headers[4] = (chttp_header){"Authorization", authorization};
  client_config = flowie_server_http_client_config(endpoint);
  tls_config = (cnet_tls_client_config){.size = sizeof(tls_config),
                                        .ca_file = endpoint->ca_file,
                                        .server_name = endpoint->server_name};
  rc = chttp_tls_profile_init(&tls_profile, &tls_config);
  if (rc != SALTS_OK) goto done;
  rc = chttp_client_init(&client, &client_config);
  if (rc != SALTS_OK) goto done;
  options = (chttp_options){.connection_uri = endpoint->connection_uri,
                            .authority = endpoint->authority,
                            .target = endpoint->target,
                            .headers = headers,
                            .header_count = 5u,
                            .body = body,
                            .body_size = body_size,
                            .timeout_ms = endpoint->timeout_ms,
                            .tls = &tls_profile,
                            .protocol = CHTTP_HTTP_1_1};
  rc = chttp_post(&client, &options, response, &error);
done:
  if (client.impl != NULL) {
    int destroy_rc = chttp_client_destroy(&client, FLOWIE_SERVER_HTTP_DESTROY_TIMEOUT_MS);
    if (rc == SALTS_OK && destroy_rc != SALTS_OK) rc = destroy_rc;
  }
  (void)chttp_tls_profile_destroy(&tls_profile);
  flowie_server_secure_clear(authorization, sizeof(authorization));
  return rc;
}

static int flowie_server_http_response_status(const chttp_response *response) {
  const char *content_type;
  if (!response) return SALTS_EIO;
  if (response->status_code == 401u || response->status_code == 403u) return SALTS_EPERM;
  if (response->status_code == 429u) return SALTS_EBUSY;
  content_type = chttp_response_header(response, "Content-Type");
  if (response->status_code != 200u || !content_type ||
      strncmp(content_type, "application/json", sizeof("application/json") - 1u) != 0)
    return SALTS_EIO;
  return SALTS_OK;
}

int flowie_server_http_headers(const flowie_server_http_provider_config_t *config,
                               char *service_id, size_t service_id_size,
                               char *service_domain, size_t service_domain_size,
                               const char *headers[4]) {
  int id_length;
  int domain_length;
  if (!config || !service_id || !service_domain || !headers) return SALTS_EINVAL;
  id_length =
      snprintf(service_id, service_id_size, "X-Flowie-Service-Id: %s", config->service_id);
  domain_length = snprintf(service_domain, service_domain_size,
                           "X-Flowie-Service-Domain: %s", config->service_domain);
  if (id_length <= 0 || (size_t)id_length >= service_id_size || domain_length <= 0 ||
      (size_t)domain_length >= service_domain_size)
    return SALTS_ERANGE;
  headers[0] = "Content-Type: application/json";
  headers[1] = "Accept: application/json";
  headers[2] = service_id;
  headers[3] = service_domain;
  return SALTS_OK;
}

static int flowie_server_http_authenticate(void *ctx,
                                           const flowie_security_auth_request_t *request,
                                           flowie_security_principal_t *principal_out) {
  flowie_server_http_security_t *security = (flowie_server_http_security_t *)ctx;
  chttp_response response = {0};
  char *body = NULL;
  size_t body_size = 0u;
  int rc;
  if (!security || !request || !principal_out || request->secret_size == 0u ||
      request->secret_size > security->auth_config.max_body_size ||
      strcmp(request->method, security->auth_config.method) != 0)
    return SALTS_EPERM;
  rc = flowie_server_http_auth_encode(request, &body, &body_size);
  if (rc != SALTS_OK) return rc;
  rc = flowie_server_http_post(&security->auth_endpoint, &security->auth_config,
                               security->auth_token, body, body_size, &response);
  if (rc != SALTS_OK) rc = SALTS_EIO;
  if (rc == SALTS_OK) rc = flowie_server_http_response_status(&response);
  if (rc == SALTS_OK)
    rc = flowie_server_http_auth_decode((const char *)response.body, response.body_size,
                                        security->auth_config.method, principal_out);
  chttp_response_destroy(&response);
  flowie_server_http_body_destroy(body, body_size, 1);
  return rc;
}

static int flowie_server_http_authorize(void *ctx, const flowie_security_request_t *request,
                                        uint64_t now_epoch_seconds,
                                        flowie_security_decision_t *decision_out) {
  flowie_server_http_security_t *security = (flowie_server_http_security_t *)ctx;
  chttp_response response = {0};
  char *body = NULL;
  size_t body_size = 0u;
  int rc;
  (void)now_epoch_seconds;
  if (!security || !request || !decision_out) return SALTS_EINVAL;
  rc = flowie_server_http_acl_encode(request, &body, &body_size);
  if (rc != SALTS_OK) return rc;
  rc = flowie_server_http_post(&security->acl_endpoint, &security->acl_config,
                               security->acl_token, body, body_size, &response);
  if (rc != SALTS_OK) rc = SALTS_EIO;
  if (rc == SALTS_OK) rc = flowie_server_http_response_status(&response);
  if (rc == SALTS_OK)
    rc = flowie_server_http_acl_decode((const char *)response.body, response.body_size,
                                       decision_out);
  chttp_response_destroy(&response);
  flowie_server_http_body_destroy(body, body_size, 0);
  return rc;
}

int flowie_server_http_security_create(const flowie_server_http_provider_config_t *auth,
                                       const flowie_server_http_provider_config_t *acl,
                                       flowie_server_http_security_t **out) {
  flowie_server_http_security_t *security;
  int rc;
  if (out) *out = NULL;
  if (!flowie_server_http_config_valid(auth, 1) ||
      !flowie_server_http_config_valid(acl, 0) || !out)
    return SALTS_EINVAL;
  security = (flowie_server_http_security_t *)calloc(1u, sizeof(*security));
  if (!security) return SALTS_ENOMEM;
  security->auth_config = *auth;
  security->acl_config = *acl;
  rc = flowie_server_http_token_copy(auth->service_token_ref, &security->auth_token);
  if (rc == SALTS_OK)
    rc = flowie_server_http_token_copy(acl->service_token_ref, &security->acl_token);
  if (rc == SALTS_OK)
    rc = flowie_server_http_endpoint_init(&security->auth_endpoint, &security->auth_config,
                                          FLOWIE_SERVER_SECURITY_RESPONSE_LIMIT);
  if (rc == SALTS_OK)
    rc = flowie_server_http_endpoint_init(&security->acl_endpoint, &security->acl_config,
                                          security->acl_config.max_body_size);
  if (rc != SALTS_OK) {
    flowie_server_http_security_destroy(security);
    return rc;
  }
  security->auth_provider = (flowie_security_auth_provider_t){
      sizeof(security->auth_provider), security, flowie_server_http_authenticate};
  security->acl_provider = (flowie_security_authorization_provider_t){
      sizeof(security->acl_provider), security, flowie_server_http_authorize};
  *out = security;
  return SALTS_OK;
}

void flowie_server_http_security_destroy(flowie_server_http_security_t *security) {
  if (!security) return;
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
