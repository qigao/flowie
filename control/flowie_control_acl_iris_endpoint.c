#include "flowie_control_acl_iris_endpoint_internal.h"

#include "flowie_mqtt_security.h"
#include "monocypher.h"
#include "platform.h"
#include "salts_error.h"
#include <json_parser.h>

#include <stdlib.h>
#include <string.h>

enum { FLOWIE_CONTROL_ACL_TOKEN_MAX = 4096 };

struct flowie_control_acl_iris_endpoint_s {
  const flowie_control_repository_t *repository;
  flowie_control_service_credential_resolver_t *service_credentials;
  size_t max_response_size;
  flowie_control_http_app_t *bound_app;
};

static int flowie_control_acl_ascii_equal(const char *left, const char *right) {
  if (!left || !right) return 0;
  while (*left && *right) {
    unsigned char a = (unsigned char)*left++;
    unsigned char b = (unsigned char)*right++;
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
    if (a != b) return 0;
  }
  return *left == '\0' && *right == '\0';
}

static int flowie_control_acl_header(const Req *req, const char *name, const char **value_out) {
  const char *found = NULL;
  size_t matches = 0u;
  if (value_out) *value_out = NULL;
  if (!req || !name || !value_out || req->headers.count < 0 ||
      (req->headers.count > 0 && !req->headers.items))
    return SALTS_EINVAL;
  for (int index = 0; index < req->headers.count; ++index) {
    const request_item_t *item = &req->headers.items[index];
    if (item->key && item->value && flowie_control_acl_ascii_equal(item->key, name)) {
      ++matches;
      found = item->value;
    }
  }
  if (matches != 1u || !found) return SALTS_EPROTO;
  *value_out = found;
  return SALTS_OK;
}

static void flowie_control_acl_wipe_authorization(Req *req) {
  if (!req || req->headers.count <= 0 || !req->headers.items) return;
  for (int index = 0; index < req->headers.count; ++index) {
    request_item_t *item = &req->headers.items[index];
    if (item->key && item->value && flowie_control_acl_ascii_equal(item->key, "Authorization")) {
      size_t size = strnlen(item->value, sizeof("Bearer ") + FLOWIE_CONTROL_ACL_TOKEN_MAX);
      if (size < sizeof("Bearer ") + FLOWIE_CONTROL_ACL_TOKEN_MAX) crypto_wipe(item->value, size);
    }
  }
}

static int flowie_control_acl_resolve_caller(
    flowie_control_acl_iris_endpoint_t *endpoint, const Req *req,
    flowie_control_verified_caller_t *caller_out) {
  static const char prefix[] = "Bearer ";
  const char *authorization = NULL;
  const char *service_domain = NULL;
  const char *service_id = NULL;
  size_t authorization_size;
  if (!endpoint || !caller_out || caller_out->size < sizeof(*caller_out)) return SALTS_EINVAL;
  if (flowie_control_acl_header(req, "Authorization", &authorization) != SALTS_OK ||
      flowie_control_acl_header(req, "X-Flowie-Service-Domain", &service_domain) != SALTS_OK ||
      flowie_control_acl_header(req, "X-Flowie-Service-Id", &service_id) != SALTS_OK)
    return SALTS_EPERM;
  authorization_size = strnlen(authorization, sizeof(prefix) + FLOWIE_CONTROL_ACL_TOKEN_MAX);
  if (authorization_size <= sizeof(prefix) - 1u ||
      authorization_size > sizeof(prefix) - 1u + FLOWIE_CONTROL_ACL_TOKEN_MAX ||
      memcmp(authorization, prefix, sizeof(prefix) - 1u) != 0)
    return SALTS_EPERM;
  return flowie_control_service_credential_resolve(
      endpoint->service_credentials, service_domain, service_id,
      (const uint8_t *)authorization + sizeof(prefix) - 1u,
      authorization_size - (sizeof(prefix) - 1u), FLOWIE_CONTROL_SERVICE_ACL_CHECK, caller_out);
}

static int flowie_control_acl_json_fields_exact(const json_value_t *object,
                                                const char *const *allowed,
                                                size_t allowed_count) {
  if (!object || json_type(object) != JSON_OBJECT ||
      json_object_size(object) != allowed_count)
    return SALTS_EPROTO;
  for (size_t index = 0u; index < json_object_size(object); ++index) {
    const char *field = json_object_key(object, index);
    int known = 0;
    for (size_t allowed_index = 0u; allowed_index < allowed_count; ++allowed_index)
      if (field && strcmp(field, allowed[allowed_index]) == 0) known = 1;
    if (!known) return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int flowie_control_acl_json_u64(const json_value_t *value, uint64_t *out) {
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

static int flowie_control_acl_copy_string(const json_value_t *object, const char *field,
                                          char *output, size_t capacity) {
  json_value_t *value = json_object_get(object, field);
  const char *text;
  size_t size;
  if (!value || json_type(value) != JSON_STRING || !output || capacity == 0u)
    return SALTS_EPROTO;
  text = json_string(value);
  size = json_string_len(value);
  if (!text || size == 0u || size >= capacity || memchr(text, '\0', size)) return SALTS_EPROTO;
  memcpy(output, text, size);
  output[size] = '\0';
  return SALTS_OK;
}

static int flowie_control_acl_copy_array(const json_value_t *object, const char *field,
                                         char *output, size_t stride, size_t maximum_size,
                                         uint32_t maximum_count, uint32_t *count_out) {
  json_value_t *array = json_object_get(object, field);
  size_t count;
  if (!array || json_type(array) != JSON_ARRAY || !output || !count_out)
    return SALTS_EPROTO;
  count = json_array_size(array);
  if (count > maximum_count) return SALTS_EPROTO;
  for (size_t index = 0u; index < count; ++index) {
    json_value_t *item = json_array_get(array, index);
    const char *text;
    size_t size;
    if (!item || json_type(item) != JSON_STRING || !(text = json_string(item)) ||
        (size = json_string_len(item)) == 0u || size > maximum_size ||
        memchr(text, '\0', size))
      return SALTS_EPROTO;
    memcpy(output + index * stride, text, size);
    output[index * stride + size] = '\0';
  }
  *count_out = (uint32_t)count;
  return SALTS_OK;
}

static int flowie_control_acl_decode_principal(const json_value_t *value,
                                               flowie_security_principal_t *principal) {
  static const char *const fields[] = {"id",      "type",   "domain", "expires_at",
                                       "policy_version", "roles", "groups"};
  uint64_t expires_at = 0u;
  uint64_t policy_version = 0u;
  if (!principal || flowie_control_acl_json_fields_exact(
                        value, fields, sizeof(fields) / sizeof(fields[0])) != SALTS_OK)
    return SALTS_EPROTO;
  *principal = (flowie_security_principal_t)FLOWIE_SECURITY_PRINCIPAL_INIT;
  if (flowie_control_acl_copy_string(value, "id", principal->principal_id,
                                     sizeof(principal->principal_id)) != SALTS_OK ||
      flowie_control_acl_copy_string(value, "type", principal->principal_type,
                                     sizeof(principal->principal_type)) != SALTS_OK ||
      flowie_control_acl_copy_string(value, "domain", principal->domain_id,
                                     sizeof(principal->domain_id)) != SALTS_OK ||
      flowie_control_acl_json_u64(json_object_get(value, "expires_at"), &expires_at) !=
          SALTS_OK ||
      flowie_control_acl_json_u64(json_object_get(value, "policy_version"),
                                  &policy_version) != SALTS_OK ||
      policy_version == 0u ||
      flowie_control_acl_copy_array(value, "roles", (char *)principal->roles,
                                    sizeof(principal->roles[0]), FLOWIE_SECURITY_TYPE_MAX,
                                    FLOWIE_SECURITY_MAX_ROLES,
                                    &principal->role_count) != SALTS_OK ||
      flowie_control_acl_copy_array(value, "groups", (char *)principal->groups,
                                    sizeof(principal->groups[0]), FLOWIE_SECURITY_ID_MAX,
                                    FLOWIE_SECURITY_MAX_GROUPS,
                                    &principal->group_count) != SALTS_OK)
    return SALTS_EPROTO;
  memcpy(principal->auth_method, "password", sizeof("password"));
  principal->scope = FLOWIE_SECURITY_SCOPE_DOMAIN;
  principal->expires_at = expires_at;
  principal->policy_version = policy_version;
  return SALTS_OK;
}

static int flowie_control_acl_decode_request(
    const char *body, size_t body_size, json_value_t **document_out,
    flowie_security_principal_t *principal_out, flowie_security_request_t *request_out,
    flowie_mqtt_security_context_t *mqtt_out) {
  static const char *const fields[] = {"version", "access", "topic", "username", "client_id",
                                       "principal"};
  json_value_t *document = NULL;
  json_value_t *access;
  json_value_t *topic;
  json_value_t *username;
  json_value_t *client_id;
  uint64_t version = 0u;
  int rc = SALTS_EPROTO;
  if (document_out) *document_out = NULL;
  if (!body || body_size == 0u || !document_out || !principal_out || !request_out || !mqtt_out)
    return SALTS_EINVAL;
  document = json_parse(body, body_size);
  if (!document)
    return SALTS_EPROTO;
  access = json_object_get(document, "access");
  topic = json_object_get(document, "topic");
  username = json_object_get(document, "username");
  client_id = json_object_get(document, "client_id");
  if (flowie_control_acl_json_fields_exact(document, fields,
                                           sizeof(fields) / sizeof(fields[0])) != SALTS_OK ||
      flowie_control_acl_json_u64(json_object_get(document, "version"), &version) !=
          SALTS_OK ||
      version != FLOWIE_CONTROL_ACL_HTTP_PROTOCOL_VERSION || !access ||
      json_type(access) != JSON_STRING || !topic ||
      json_type(topic) != JSON_STRING || json_string_len(topic) == 0u ||
      json_string_len(topic) > FLOWIE_CONTROL_ACL_HTTP_DEFAULT_RESPONSE_MAX || !username ||
      json_type(username) != JSON_STRING ||
      json_string_len(username) > FLOWIE_SECURITY_ID_MAX || !client_id ||
      json_type(client_id) != JSON_STRING ||
      json_string_len(client_id) > FLOWIE_SECURITY_ID_MAX ||
      flowie_control_acl_decode_principal(json_object_get(document, "principal"),
                                          principal_out) != SALTS_OK)
    goto done;
  *request_out = (flowie_security_request_t)FLOWIE_SECURITY_REQUEST_INIT;
  request_out->principal = principal_out;
  request_out->domain_id = principal_out->domain_id;
  request_out->resource = json_string(topic);
  if (strcmp(json_string(access), "connect") == 0) {
    request_out->action = FLOWIE_SECURITY_ACTION_CONNECT;
    request_out->resource_type = FLOWIE_SECURITY_RESOURCE_GENERIC;
  } else if (strcmp(json_string(access), "read") == 0 ||
             strcmp(json_string(access), "write") == 0) {
    request_out->action = strcmp(json_string(access), "read") == 0
                              ? FLOWIE_SECURITY_ACTION_SUBSCRIBE
                              : FLOWIE_SECURITY_ACTION_PUBLISH;
    request_out->resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
    *mqtt_out = (flowie_mqtt_security_context_t)FLOWIE_MQTT_SECURITY_CONTEXT_INIT;
    mqtt_out->kind = request_out->action == FLOWIE_SECURITY_ACTION_SUBSCRIBE
                         ? FLOWIE_MQTT_SECURITY_TOPIC_FILTER
                         : FLOWIE_MQTT_SECURITY_TOPIC;
    mqtt_out->username =
        (flowie_mqtt_span_t){(const uint8_t *)json_string(username),
                             json_string_len(username)};
    mqtt_out->client_id =
        (flowie_mqtt_span_t){(const uint8_t *)json_string(client_id),
                             json_string_len(client_id)};
    request_out->protocol_context = mqtt_out;
  } else {
    goto done;
  }
  rc = SALTS_OK;
  *document_out = document;
  document = NULL;

done:
  json_free(document);
  return rc;
}

static const char *flowie_control_acl_reason(flowie_security_decision_reason_t reason) {
  switch (reason) {
  case FLOWIE_SECURITY_REASON_ALLOW_RULE: return "allow_rule";
  case FLOWIE_SECURITY_REASON_DENY_RULE: return "deny_rule";
  case FLOWIE_SECURITY_REASON_DOMAIN_MISMATCH: return "domain_mismatch";
  case FLOWIE_SECURITY_REASON_PRINCIPAL_EXPIRED: return "principal_expired";
  case FLOWIE_SECURITY_REASON_POLICY_VERSION_MISMATCH: return "policy_version_mismatch";
  default: return "default_deny";
  }
}

static void flowie_control_acl_free_value(json_value_t *value) {
  json_value_t *owned = (json_value_t *)value;
  if (owned) json_free(owned);
}

static int flowie_control_acl_add(json_value_t *object, const char *field, json_value_t *value) {
  if (value && json_object_add_checked(object, field, value)) return SALTS_OK;
  flowie_control_acl_free_value(value);
  return SALTS_ENOMEM;
}

static int flowie_control_acl_encode_decision(const flowie_security_decision_t *decision,
                                              char **body_out, size_t *body_size_out) {
  json_value_t *document = NULL;
  int rc = SALTS_ENOMEM;
  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!decision || decision->size < sizeof(*decision) || decision->policy_version == 0u ||
      !body_out || !body_size_out)
    return SALTS_EINVAL;
  document = (json_value_t *)json_create_object();
  if (!document) return SALTS_ENOMEM;
  if (flowie_control_acl_add(document, "version",
                             json_create_uint64(FLOWIE_CONTROL_ACL_HTTP_PROTOCOL_VERSION)) !=
          SALTS_OK ||
      flowie_control_acl_add(document, "allowed",
                             json_create_bool(decision->effect ==
                                                    FLOWIE_SECURITY_ALLOW)) != SALTS_OK ||
      flowie_control_acl_add(document, "reason",
                             json_create_string(flowie_control_acl_reason(decision->reason))) !=
          SALTS_OK ||
      flowie_control_acl_add(document, "policy_version",
                             json_create_uint64(decision->policy_version)) != SALTS_OK)
    goto done;
  *body_out = json_serialize(document, body_size_out);
  if (!*body_out) goto done;
  rc = SALTS_OK;
done:
  json_free(document);
  return rc;
}

static int flowie_control_acl_status(int rc) {
  if (rc == SALTS_EPERM) return FORBIDDEN;
  if (rc == SALTS_EPROTO || rc == SALTS_EINVAL) return BAD_REQUEST;
  return SERVICE_UNAVAILABLE;
}

int flowie_control_acl_iris_endpoint_process(flowie_control_acl_iris_endpoint_t *endpoint, Req *req,
                                             int *status_out, char **body_out,
                                             size_t *body_size_out) {
  json_value_t *document = NULL;
  flowie_security_policy_bundle_t bundle = FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
  flowie_control_policy_status_t policy = FLOWIE_CONTROL_POLICY_STATUS_INIT;
  flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
  flowie_security_request_t request = FLOWIE_SECURITY_REQUEST_INIT;
  flowie_mqtt_security_context_t mqtt = FLOWIE_MQTT_SECURITY_CONTEXT_INIT;
  flowie_security_matcher_t matcher = FLOWIE_SECURITY_MATCHER_INIT;
  flowie_security_realm_config_t realm_config = FLOWIE_SECURITY_REALM_CONFIG_INIT;
  flowie_security_realm_t *realm = NULL;
  flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;
  flowie_control_verified_caller_t caller = FLOWIE_CONTROL_VERIFIED_CALLER_INIT;
  const char *content_type = NULL;
  uint64_t now;
  int rc = SALTS_EPROTO;
  if (status_out) *status_out = INTERNAL_SERVER_ERROR;
  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!endpoint || !req || !status_out || !body_out || !body_size_out) return SALTS_EINVAL;
  if (!req->method || strcmp(req->method, "POST") != 0 || req->body_stream || !req->body ||
      req->body_len == 0u || req->body_len > endpoint->max_response_size ||
      flowie_control_acl_header(req, "Content-Type", &content_type) != SALTS_OK ||
      !flowie_control_acl_ascii_equal(content_type, "application/json"))
    goto done;
  rc = flowie_control_acl_resolve_caller(endpoint, req, &caller);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_acl_decode_request(req->body, req->body_len, &document, &principal, &request,
                                         &mqtt);
  if (rc != SALTS_OK) goto done;
  now = salts_realtime_ms() / 1000u;
  if (now == 0u) {
    rc = SALTS_EIO;
    goto done;
  }
  decision.policy_version = principal.policy_version;
  if (principal.expires_at != 0u && now >= principal.expires_at) {
    decision.reason = FLOWIE_SECURITY_REASON_PRINCIPAL_EXPIRED;
    rc = flowie_control_acl_encode_decision(&decision, body_out, body_size_out);
    goto done;
  }
  rc = endpoint->repository->policy->status(endpoint->repository->ctx, principal.domain_id,
                                            &policy);
  if (rc != SALTS_OK) goto done;
  if (policy.policy_version != principal.policy_version ||
      (policy.expires_at != 0u && now >= policy.expires_at)) {
    decision.reason = FLOWIE_SECURITY_REASON_POLICY_VERSION_MISMATCH;
    rc = flowie_control_acl_encode_decision(&decision, body_out, body_size_out);
    goto done;
  }
  rc = endpoint->repository->policy->bundle_load(endpoint->repository->ctx, principal.domain_id,
                                                 principal.policy_version, &bundle);
  if (rc != SALTS_OK) goto done;
  rc = flowie_mqtt_security_matcher_init(&matcher);
  if (rc != SALTS_OK) goto done;
  realm_config.resource_uid = "flowie-control-acl-check";
  realm_config.owner_name = "flowie-control";
  realm_config.policy_version = bundle.policy_version;
  realm_config.rules = bundle.rules;
  realm_config.rule_count = bundle.rule_count;
  realm_config.matcher = matcher;
  rc = flowie_security_realm_create(&realm_config, &realm);
  if (rc != SALTS_OK) goto done;
  rc = flowie_security_realm_evaluate(realm, &request, now, &decision);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_acl_encode_decision(&decision, body_out, body_size_out);

done:
  flowie_security_realm_destroy(realm);
  if (bundle.provider_bundle)
    endpoint->repository->policy->bundle_release(endpoint->repository->ctx, &bundle);
  json_free(document);
  flowie_control_acl_wipe_authorization(req);
  *status_out = rc == SALTS_OK ? OK : flowie_control_acl_status(rc);
  return SALTS_OK;
}

static void flowie_control_acl_handle(flowie_control_acl_iris_endpoint_t *endpoint, Req *req,
                                      Res *res) {
  static const char unavailable[] = "{\"version\":4,\"error\":\"unavailable\"}";
  char *body = NULL;
  size_t body_size = 0u;
  int status = INTERNAL_SERVER_ERROR;
  if (!res) return;
  set_header(res, "Cache-Control", "no-store");
  if (flowie_control_acl_iris_endpoint_process(endpoint, req, &status, &body, &body_size) !=
      SALTS_OK || !body) {
    reply(res, status, "application/json", unavailable, sizeof(unavailable) - 1u);
    return;
  }
  reply(res, status, "application/json", body, body_size);
  json_serialize_free(body);
}

static void flowie_control_acl_registered_handler(Req *req, Res *res) {
  flowie_control_acl_iris_endpoint_t *endpoint = NULL;
  if (req && req->app)
    endpoint = (flowie_control_acl_iris_endpoint_t *)flowie_control_http_app_lookup_context(
        req->app, FLOWIE_CONTROL_ACL_HTTP_PATH);
  flowie_control_acl_handle(endpoint, req, res);
}

int flowie_control_acl_iris_endpoint_create(const flowie_control_acl_iris_endpoint_config_t *config,
                                            flowie_control_acl_iris_endpoint_t **out) {
  flowie_control_acl_iris_endpoint_t *endpoint;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) ||
      flowie_control_repository_validate(config->repository) != SALTS_OK ||
      !config->service_credentials || config->max_response_size == 0u || !out)
    return SALTS_EINVAL;
  endpoint = (flowie_control_acl_iris_endpoint_t *)calloc(1u, sizeof(*endpoint));
  if (!endpoint) return SALTS_ENOMEM;
  endpoint->repository = config->repository;
  endpoint->service_credentials = config->service_credentials;
  endpoint->max_response_size = config->max_response_size;
  *out = endpoint;
  return SALTS_OK;
}

void flowie_control_acl_iris_endpoint_destroy(flowie_control_acl_iris_endpoint_t *endpoint) {
  if (!endpoint) return;
  if (endpoint->bound_app) {
    (void)flowie_control_http_app_unpost(endpoint->bound_app, FLOWIE_CONTROL_ACL_HTTP_PATH,
                                         flowie_control_acl_registered_handler);
    (void)flowie_control_http_app_unbind_context(endpoint->bound_app,
                                                 FLOWIE_CONTROL_ACL_HTTP_PATH, endpoint);
  }
  crypto_wipe(endpoint, sizeof(*endpoint));
  free(endpoint);
}

int flowie_control_acl_iris_endpoint_register(flowie_control_acl_iris_endpoint_t *endpoint,
                                              flowie_control_http_app_t *app) {
  if (!endpoint || !app || endpoint->bound_app) return SALTS_EINVAL;
  if (flowie_control_http_app_bind_context(app, FLOWIE_CONTROL_ACL_HTTP_PATH, endpoint) != SALTS_OK)
    return SALTS_EBUSY;
  endpoint->bound_app = app;
  if (flowie_control_http_app_post(app, FLOWIE_CONTROL_ACL_HTTP_PATH,
                                   flowie_control_acl_registered_handler) != SALTS_OK) {
    endpoint->bound_app = NULL;
    (void)flowie_control_http_app_unbind_context(app, FLOWIE_CONTROL_ACL_HTTP_PATH, endpoint);
    return SALTS_EBUSY;
  }
  return SALTS_OK;
}
