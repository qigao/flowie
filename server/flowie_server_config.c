#include "flowie_server_config_internal.h"

#include "flowie.h"

#include "salts_error.h"
#include "salts_fs.h"
#include <cyaml/cyaml.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FLOWIE_SERVER_CONFIG_MAX_BYTES = 1024u * 1024u };

struct flowie_server_config_s {
  flowie_endpoint_config_t endpoint;
  char endpoint_name[FLOWIE_SECURITY_ID_MAX + 1u];
  char host[256];
  char path[256];
  char realm_name[FLOWIE_SECURITY_ID_MAX + 1u];
  char realm_resource_uid[FLOWIE_SECURITY_ID_MAX + 1u];
  char realm_owner_name[FLOWIE_SECURITY_ID_MAX + 1u];
  char acl_provider_name[FLOWIE_SECURITY_ID_MAX + 1u];
  char auth_method[FLOWIE_SECURITY_TYPE_MAX + 1u];
  flowie_server_http_provider_config_t auth;
  flowie_server_http_provider_config_t acl;
  int security_enabled;
};

static int flowie_server_config_fail(flowie_server_config_error_t *error, int status,
                                     const char *path, const char *message) {
  if (error && error->size >= sizeof(*error)) {
    error->status = status;
    (void)snprintf(error->path, sizeof(error->path), "%s", path ? path : "$");
    (void)snprintf(error->message, sizeof(error->message), "%s",
                   message ? message : "invalid Flowie configuration");
  }
  return status;
}

static cyaml_node_t *flowie_yaml_mapping(const cyaml_doc_t *doc,
                                              cyaml_node_t *parent, const char *key) {
  cyaml_node_t *node = cyaml_get(doc, parent, key);
  return cyaml_is_map(node) ? node : NULL;
}

static int flowie_yaml_string(const cyaml_doc_t *doc, cyaml_node_t *mapping,
                              const char *key, char *out, size_t capacity, int required) {
  cyaml_node_t *node;
  char *value;
  size_t length;
  if (!doc || !mapping || !key || !out || capacity == 0u) return SALTS_EINVAL;
  node = cyaml_get(doc, mapping, key);
  if (!node) return required ? SALTS_ENOENT : SALTS_OK;
  if (!cyaml_is_scalar(node)) return SALTS_EPROTO;
  value = cyaml_scalar_str(doc, node);
  if (!value) return SALTS_ENOMEM;
  length = strlen(value);
  if ((required && length == 0u) || length >= capacity) {
    free(value);
    return length >= capacity ? SALTS_ENOSPC : SALTS_EINVAL;
  }
  memcpy(out, value, length + 1u);
  free(value);
  return SALTS_OK;
}

static int flowie_yaml_u64(const cyaml_doc_t *doc, cyaml_node_t *mapping,
                           const char *key, uint64_t *out, int required) {
  cyaml_node_t *node;
  char *text;
  char *end = NULL;
  unsigned long long value;
  if (!doc || !mapping || !key || !out) return SALTS_EINVAL;
  node = cyaml_get(doc, mapping, key);
  if (!node) return required ? SALTS_ENOENT : SALTS_OK;
  if (!cyaml_is_scalar(node)) return SALTS_EPROTO;
  text = cyaml_scalar_str(doc, node);
  if (!text) return SALTS_ENOMEM;
  errno = 0;
  value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    free(text);
    return SALTS_EPROTO;
  }
  free(text);
  *out = (uint64_t)value;
  return SALTS_OK;
}

static int flowie_yaml_bool(const cyaml_doc_t *doc, cyaml_node_t *mapping,
                            const char *key, int *out) {
  char value[8] = {0};
  int rc = flowie_yaml_string(doc, mapping, key, value, sizeof(value), 0);
  if (rc != SALTS_OK || !value[0]) return rc;
  if (strcmp(value, "true") == 0) *out = 1;
  else if (strcmp(value, "false") == 0) *out = 0;
  else return SALTS_EPROTO;
  return SALTS_OK;
}

static int flowie_server_endpoint_number(const cyaml_doc_t *doc,
                                         cyaml_node_t *mapping, const char *key,
                                         uint64_t maximum, uint64_t *target) {
  uint64_t value = UINT64_MAX;
  int rc = flowie_yaml_u64(doc, mapping, key, &value, 0);
  if (rc != SALTS_OK || value == UINT64_MAX) return rc;
  if (value > maximum) return SALTS_ERANGE;
  *target = value;
  return SALTS_OK;
}

static int flowie_server_parse_endpoint(const cyaml_doc_t *doc, cyaml_node_t *mapping,
                                        flowie_server_config_t *config) {
  char transport[16] = {0};
  uint64_t number;
  int rc;
  config->endpoint = (flowie_endpoint_config_t)FLOWIE_ENDPOINT_CONFIG_INIT;
  config->endpoint.host = config->host;
  config->endpoint.path = config->path;
  config->endpoint.max_packet_size = FLOWIE_DEFAULT_MAX_PACKET_SIZE;
  config->endpoint.max_connections = FLOWIE_DEFAULT_MAX_CONNECTIONS;
  config->endpoint.max_sessions = FLOWIE_DEFAULT_MAX_CONNECTIONS;
  config->endpoint.max_subscriptions_per_session = FLOWIE_DEFAULT_MAX_SUBSCRIPTIONS_PER_SESSION;
  config->endpoint.max_inflight_per_session = FLOWIE_DEFAULT_MAX_INFLIGHT_PER_SESSION;
  config->endpoint.max_retained_messages = FLOWIE_DEFAULT_MAX_CONNECTIONS;
  config->endpoint.send_hwm_bytes = FLOWIE_DEFAULT_SEND_HWM_BYTES;
  config->endpoint.manage_sessions = 1;
  (void)snprintf(config->host, sizeof(config->host), "%s", "0.0.0.0");
  (void)snprintf(config->path, sizeof(config->path), "%s", "/mqtt");
  rc = flowie_yaml_string(doc, mapping, "transport", transport, sizeof(transport), 1);
  if (rc != SALTS_OK) return rc;
  if (strcmp(transport, "tcp") == 0) config->endpoint.transport = FLOWIE_TRANSPORT_TCP;
  else if (strcmp(transport, "tls") == 0) config->endpoint.transport = FLOWIE_TRANSPORT_TLS;
  else if (strcmp(transport, "ws") == 0) config->endpoint.transport = FLOWIE_TRANSPORT_WS;
  else if (strcmp(transport, "wss") == 0) config->endpoint.transport = FLOWIE_TRANSPORT_WSS;
  else return SALTS_ENOTSUP;
  rc = flowie_yaml_string(doc, mapping, "host", config->host, sizeof(config->host), 1);
  if (rc != SALTS_OK) return rc;
  rc = flowie_yaml_string(doc, mapping, "path", config->path, sizeof(config->path), 0);
  if (rc != SALTS_OK) return rc;
#define FLOWIE_ENDPOINT_U64(key, maximum, member)                                                \
  do {                                                                                            \
    number = UINT64_MAX;                                                                          \
    rc = flowie_server_endpoint_number(doc, mapping, key, maximum, &number);                      \
    if (rc != SALTS_OK) return rc;                                                                 \
    if (number != UINT64_MAX) config->endpoint.member = number;                                  \
  } while (0)
  FLOWIE_ENDPOINT_U64("port", UINT16_MAX, port);
  FLOWIE_ENDPOINT_U64("max_packet_size", FLOWIE_MQTT_MAX_WIRE_PACKET_SIZE, max_packet_size);
  FLOWIE_ENDPOINT_U64("max_connections", FLOWIE_MAX_CONNECTIONS_LIMIT, max_connections);
  FLOWIE_ENDPOINT_U64("max_sessions", SIZE_MAX, max_sessions);
  FLOWIE_ENDPOINT_U64("max_retained_messages", SIZE_MAX, max_retained_messages);
  FLOWIE_ENDPOINT_U64("max_subscriptions_per_session", UINT16_MAX,
                      max_subscriptions_per_session);
  FLOWIE_ENDPOINT_U64("max_inflight_per_session", UINT16_MAX, max_inflight_per_session);
  FLOWIE_ENDPOINT_U64("send_hwm_bytes", SIZE_MAX, send_hwm_bytes);
  FLOWIE_ENDPOINT_U64("coroutine_stack_size", FLOWIE_MAX_COROUTINE_STACK_SIZE,
                      coroutine_stack_size);
  FLOWIE_ENDPOINT_U64("stream_recv_buffer_bytes", FLOWIE_MAX_RECV_BUFFER_SIZE,
                      stream_recv_buffer_bytes);
  FLOWIE_ENDPOINT_U64("topic_alias_maximum", UINT16_MAX, topic_alias_maximum);
#undef FLOWIE_ENDPOINT_U64
  rc = flowie_yaml_bool(doc, mapping, "manage_sessions", &config->endpoint.manage_sessions);
  if (rc != SALTS_OK) return rc;
  if (config->endpoint.port <= 0 || config->endpoint.max_packet_size < 2u ||
      config->endpoint.max_connections == 0u || config->endpoint.max_sessions == 0u ||
      config->endpoint.max_subscriptions_per_session == 0u ||
      config->endpoint.max_inflight_per_session == 0u ||
      (config->endpoint.coroutine_stack_size != 0u &&
       config->endpoint.coroutine_stack_size < FLOWIE_MIN_COROUTINE_STACK_SIZE) ||
      (config->endpoint.stream_recv_buffer_bytes != 0u &&
       config->endpoint.stream_recv_buffer_bytes < FLOWIE_MIN_RECV_BUFFER_SIZE))
    return SALTS_ERANGE;
  return SALTS_OK;
}

static int flowie_server_parse_http_provider(const cyaml_doc_t *doc,
                                             cyaml_node_t *channels, const char *name,
                                             int auth_provider,
                                             flowie_server_http_provider_config_t *out) {
  cyaml_node_t *channel = flowie_yaml_mapping(doc, channels, name);
  cyaml_node_t *config;
  cyaml_node_t *tls;
  char kind[32] = {0};
  char backend[16] = {0};
  uint64_t number = 0u;
  int rc;
  if (!channel) return SALTS_ENOENT;
  rc = flowie_yaml_string(doc, channel, "kind", kind, sizeof(kind), 1);
  if (rc != SALTS_OK || strcmp(kind, auth_provider ? "auth_provider" : "acl_provider") != 0)
    return rc == SALTS_OK ? SALTS_EPROTO : rc;
  config = flowie_yaml_mapping(doc, channel, "config");
  if (!config) return SALTS_EPROTO;
  rc = flowie_yaml_string(doc, config, "backend", backend, sizeof(backend), 1);
  if (rc != SALTS_OK || strcmp(backend, "https") != 0)
    return rc == SALTS_OK ? SALTS_ENOTSUP : rc;
  if ((rc = flowie_yaml_string(doc, config, "url", out->url, sizeof(out->url), 1)) != SALTS_OK ||
      strncmp(out->url, "https://", 8u) != 0)
    return rc == SALTS_OK ? SALTS_EPERM : rc;
  if (auth_provider &&
      (rc = flowie_yaml_string(doc, config, "method", out->method, sizeof(out->method), 1)) !=
          SALTS_OK)
    return rc;
  if ((rc = flowie_yaml_string(doc, config, "service_id", out->service_id,
                               sizeof(out->service_id), 1)) != SALTS_OK ||
      (rc = flowie_yaml_string(doc, config, "service_domain", out->service_domain,
                               sizeof(out->service_domain), 1)) != SALTS_OK ||
      (rc = flowie_yaml_string(doc, config, "service_token_ref", out->service_token_ref,
                               sizeof(out->service_token_ref), 1)) != SALTS_OK)
    return rc;
  if (strncmp(out->service_token_ref, "env://", 6u) != 0 || !out->service_token_ref[6])
    return SALTS_EPERM;
  if ((rc = flowie_yaml_u64(doc, config, "timeout_ms", &number, 1)) != SALTS_OK || number == 0u ||
      number > UINT32_MAX)
    return rc == SALTS_OK ? SALTS_ERANGE : rc;
  out->timeout_ms = (uint32_t)number;
  number = 0u;
  rc = flowie_yaml_u64(doc, config, auth_provider ? "max_secret_size" : "max_response_size",
                       &number, 1);
  if (rc != SALTS_OK || number == 0u || number > SIZE_MAX)
    return rc == SALTS_OK ? SALTS_ERANGE : rc;
  out->max_body_size = (size_t)number;
  tls = flowie_yaml_mapping(doc, config, "tls");
  if (!tls) return SALTS_EPERM;
  return flowie_yaml_string(doc, tls, "ca_file", out->ca_file, sizeof(out->ca_file), 1);
}

static int flowie_server_parse_security(const cyaml_doc_t *doc, cyaml_node_t *profile,
                                        cyaml_node_t *channels, cyaml_node_t *endpoint,
                                        int required, flowie_server_config_t *config) {
  char auth_name[FLOWIE_SECURITY_ID_MAX + 1u] = {0};
  char acl_name[FLOWIE_SECURITY_ID_MAX + 1u] = {0};
  cyaml_node_t *realm;
  cyaml_node_t *realm_config;
  int rc = flowie_yaml_string(doc, profile, "auth_provider", auth_name, sizeof(auth_name), 0);
  if (rc != SALTS_OK) return rc;
  rc = flowie_yaml_string(doc, endpoint, "security_realm", config->realm_name,
                          sizeof(config->realm_name), 0);
  if (rc != SALTS_OK) return rc;
  rc = flowie_yaml_string(doc, endpoint, "auth_method", config->auth_method,
                          sizeof(config->auth_method), 0);
  if (rc != SALTS_OK) return rc;
  if (!auth_name[0] && !config->realm_name[0] && !config->auth_method[0])
    return required ? SALTS_EINVAL : SALTS_OK;
  if (!auth_name[0] || !config->realm_name[0] || !config->auth_method[0]) return SALTS_EINVAL;
  realm = flowie_yaml_mapping(doc, channels, config->realm_name);
  if (!realm) return SALTS_ENOENT;
  realm_config = flowie_yaml_mapping(doc, realm, "config");
  if (!realm_config) return SALTS_EPROTO;
  rc = flowie_yaml_string(doc, realm_config, "resource_uid", config->realm_resource_uid,
                          sizeof(config->realm_resource_uid), 1);
  if (rc != SALTS_OK) return rc;
  rc = flowie_yaml_string(doc, realm_config, "owner_name", config->realm_owner_name,
                          sizeof(config->realm_owner_name), 1);
  if (rc != SALTS_OK) return rc;
  rc = flowie_yaml_string(doc, realm_config, "policy_source", config->acl_provider_name,
                          sizeof(config->acl_provider_name), 1);
  if (rc != SALTS_OK) return rc;
  (void)snprintf(acl_name, sizeof(acl_name), "%s", config->acl_provider_name);
  rc = flowie_server_parse_http_provider(doc, channels, auth_name, 1, &config->auth);
  if (rc != SALTS_OK) return rc;
  if (strcmp(config->auth.method, config->auth_method) != 0) return SALTS_EPROTO;
  rc = flowie_server_parse_http_provider(doc, channels, acl_name, 0, &config->acl);
  if (rc == SALTS_OK) config->security_enabled = 1;
  return rc;
}

int flowie_server_config_load(const char *path, const char *profile_name, int require_security,
                              flowie_server_config_t **out,
                              flowie_server_config_error_t *error) {
  salts_fs_buf_t bytes = {0};
  cyaml_doc_t *doc = NULL;
  cyaml_node_t *root;
  cyaml_node_t *profiles;
  cyaml_node_t *profile;
  cyaml_node_t *adapters;
  cyaml_node_t *adapter;
  cyaml_node_t *endpoint;
  cyaml_node_t *channels;
  flowie_server_config_t *config = NULL;
  int rc;
  if (out) *out = NULL;
  if (error && error->size >= sizeof(*error))
    *error = (flowie_server_config_error_t)FLOWIE_SERVER_CONFIG_ERROR_INIT;
  if (!path || !path[0] || !profile_name || !profile_name[0] || !out)
    return flowie_server_config_fail(error, SALTS_EINVAL, "$", "path and profile are required");
  rc = salts_fs_read_file(path, &bytes);
  if (rc != SALTS_OK)
    return flowie_server_config_fail(error, rc, "$", "cannot read Flowie configuration");
  if (bytes.len == 0u || bytes.len > FLOWIE_SERVER_CONFIG_MAX_BYTES) {
    salts_fs_buf_free(&bytes);
    return flowie_server_config_fail(error, SALTS_ERANGE, "$", "configuration size is invalid");
  }
  doc = cyaml_parse(bytes.base, bytes.len, NULL, NULL);
  if (!doc) {
    salts_fs_buf_free(&bytes);
    return flowie_server_config_fail(error, SALTS_EPROTO, "$", "configuration is not valid YAML");
  }
  root = cyaml_root(doc);
  profiles = flowie_yaml_mapping(doc, root, "profiles");
  profile = profiles ? flowie_yaml_mapping(doc, profiles, profile_name) : NULL;
  adapters = flowie_yaml_mapping(doc, root, "adapters");
  channels = flowie_yaml_mapping(doc, root, "channels");
  if (!profile || !adapters || !channels) {
    rc = flowie_server_config_fail(error, SALTS_ENOENT, "$.profiles",
                                   "profile, adapters, or channels mapping is missing");
    goto done;
  }
  config = (flowie_server_config_t *)calloc(1u, sizeof(*config));
  if (!config) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  rc = flowie_yaml_string(doc, profile, "endpoint", config->endpoint_name,
                          sizeof(config->endpoint_name), 1);
  adapter = rc == SALTS_OK ? flowie_yaml_mapping(doc, adapters, config->endpoint_name) : NULL;
  endpoint = adapter ? flowie_yaml_mapping(doc, adapter, "config") : NULL;
  if (rc != SALTS_OK || !endpoint) {
    rc = flowie_server_config_fail(error, rc == SALTS_OK ? SALTS_ENOENT : rc,
                                   "$.profiles.*.endpoint", "endpoint adapter is missing");
    goto done;
  }
  rc = flowie_server_parse_endpoint(doc, endpoint, config);
  if (rc != SALTS_OK) {
    rc = flowie_server_config_fail(error, rc, "$.adapters.*.config",
                                   "endpoint configuration is invalid");
    goto done;
  }
  rc = flowie_server_parse_security(doc, profile, channels, endpoint, require_security, config);
  if (rc != SALTS_OK) {
    rc = flowie_server_config_fail(error, rc, "$.profiles.*.auth_provider",
                                   "complete HTTPS Auth and ACL configuration is required");
    goto done;
  }
  *out = config;
  config = NULL;
  rc = SALTS_OK;

done:
  free(config);
  cyaml_free(doc);
  salts_fs_buf_free(&bytes);
  return rc;
}

void flowie_server_config_destroy(flowie_server_config_t *config) { free(config); }

const flowie_endpoint_config_t *flowie_server_config_endpoint(
    const flowie_server_config_t *config) {
  return config ? &config->endpoint : NULL;
}

const char *flowie_server_config_endpoint_name(const flowie_server_config_t *config) {
  return config ? config->endpoint_name : NULL;
}

const char *flowie_server_config_realm_name(const flowie_server_config_t *config) {
  return config && config->security_enabled ? config->realm_name : NULL;
}

const char *flowie_server_config_realm_resource_uid(const flowie_server_config_t *config) {
  return config && config->security_enabled ? config->realm_resource_uid : NULL;
}

const char *flowie_server_config_realm_owner_name(const flowie_server_config_t *config) {
  return config && config->security_enabled ? config->realm_owner_name : NULL;
}

const char *flowie_server_config_acl_provider_name(const flowie_server_config_t *config) {
  return config && config->security_enabled ? config->acl_provider_name : NULL;
}

const char *flowie_server_config_auth_method(const flowie_server_config_t *config) {
  return config && config->security_enabled ? config->auth_method : NULL;
}

const flowie_server_http_provider_config_t *flowie_server_config_auth(
    const flowie_server_config_t *config) {
  return config && config->security_enabled ? &config->auth : NULL;
}

const flowie_server_http_provider_config_t *flowie_server_config_acl(
    const flowie_server_config_t *config) {
  return config && config->security_enabled ? &config->acl : NULL;
}
