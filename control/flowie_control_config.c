#include "flowie_control_config_internal.h"

#include "turbo_error.h"
#include "turbo_parser.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int control_config_error(flowie_control_config_error_t *error, int status, const char *path,
                                const char *message) {
  if (error && error->size >= sizeof(*error)) {
    *error = (flowie_control_config_error_t)FLOWIE_CONTROL_CONFIG_ERROR_INIT;
    error->status = status;
    (void)snprintf(error->path, sizeof(error->path), "%s", path ? path : "$");
    (void)snprintf(error->message, sizeof(error->message), "%s", message ? message : "error");
  }
  return status;
}

static int control_config_key_allowed(const char *key, const char *const *allowed, size_t count) {
  for (size_t index = 0u; index < count; ++index) {
    if (strcmp(key, allowed[index]) == 0) return 1;
  }
  return 0;
}

static int control_config_object(const json_value_t *value, const char *path,
                                 const char *const *allowed, size_t allowed_count,
                                 flowie_control_config_error_t *error) {
  if (!value || turbo_json_type(value) != TURBO_JSON_OBJECT)
    return control_config_error(error, TURBO_EINVAL, path, "expected mapping");
  for (size_t index = 0u; index < turbo_json_object_size(value); ++index) {
    const char *key = turbo_json_object_key(value, index);
    if (!key || !control_config_key_allowed(key, allowed, allowed_count)) {
      char field[FLOWIE_CONTROL_CONFIG_ERROR_PATH_MAX + 1u];
      (void)snprintf(field, sizeof(field), "%s.%s", path, key ? key : "?");
      return control_config_error(error, TURBO_EINVAL, field, "unknown field");
    }
    for (size_t prior = 0u; prior < index; ++prior) {
      const char *prior_key = turbo_json_object_key(value, prior);
      if (prior_key && strcmp(prior_key, key) == 0) {
        char field[FLOWIE_CONTROL_CONFIG_ERROR_PATH_MAX + 1u];
        (void)snprintf(field, sizeof(field), "%s.%s", path, key);
        return control_config_error(error, TURBO_EALREADY, field, "field appears more than once");
      }
    }
  }
  return TURBO_OK;
}

static int control_config_text(const json_value_t *value, const char *path, char *destination,
                               size_t capacity, int required,
                               flowie_control_config_error_t *error) {
  const char *source;
  size_t length;
  if (!destination || capacity == 0u) return TURBO_EINVAL;
  destination[0] = '\0';
  if (!value)
    return required ? control_config_error(error, TURBO_EINVAL, path, "required field missing")
                    : TURBO_OK;
  if (turbo_json_type(value) != TURBO_JSON_STRING)
    return control_config_error(error, TURBO_EINVAL, path, "expected string");
  source = turbo_json_string(value);
  length = source ? strnlen(source, capacity) : 0u;
  if (length == 0u)
    return required ? control_config_error(error, TURBO_EINVAL, path, "empty string") : TURBO_OK;
  if (length >= capacity)
    return control_config_error(error, TURBO_ENAMETOOLONG, path, "string exceeds limit");
  for (size_t index = 0u; index < length; ++index) {
    unsigned char byte = (unsigned char)source[index];
    if (byte < 0x20u || byte == 0x7fu)
      return control_config_error(error, TURBO_EINVAL, path, "control character is not allowed");
  }
  memcpy(destination, source, length + 1u);
  return TURBO_OK;
}

static int control_config_integer(const json_value_t *value, const char *path, uint64_t minimum,
                                  uint64_t maximum, uint64_t *out,
                                  flowie_control_config_error_t *error) {
  double number;
  uint64_t converted;
  if (!value || !out) return TURBO_EINVAL;
  if (turbo_json_type(value) != TURBO_JSON_NUMBER)
    return control_config_error(error, TURBO_EINVAL, path, "expected integer");
  number = turbo_json_number(value);
  if (!isfinite(number) || number < (double)minimum || number > (double)maximum)
    return control_config_error(error, TURBO_ERANGE, path, "integer is outside supported range");
  converted = (uint64_t)number;
  if ((double)converted != number)
    return control_config_error(error, TURBO_EINVAL, path, "expected integer");
  *out = converted;
  return TURBO_OK;
}

static int control_config_boolean(const json_value_t *value, const char *path, int *out,
                                  flowie_control_config_error_t *error) {
  if (!value || !out) return TURBO_EINVAL;
  if (turbo_json_type(value) != TURBO_JSON_BOOL)
    return control_config_error(error, TURBO_EINVAL, path, "expected boolean");
  *out = turbo_json_bool(value) ? 1 : 0;
  return TURBO_OK;
}

static int control_config_fingerprint_valid(const char *value) {
  static const char prefix[] = "sha256:";
  if (!value || strlen(value) != FLOWIE_CONTROL_AUTH_CERT_SHA256_TEXT_SIZE ||
      memcmp(value, prefix, sizeof(prefix) - 1u) != 0)
    return 0;
  for (size_t index = sizeof(prefix) - 1u; index < FLOWIE_CONTROL_AUTH_CERT_SHA256_TEXT_SIZE;
       ++index) {
    char byte = value[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return 0;
  }
  return 1;
}

int flowie_control_config_secret_ref_valid(const char *value) {
  static const char prefix[] = "env://";
  const char *name;
  if (!value || strncmp(value, prefix, sizeof(prefix) - 1u) != 0) return 0;
  name = value + sizeof(prefix) - 1u;
  if (!(name[0] == '_' || (name[0] >= 'A' && name[0] <= 'Z'))) return 0;
  for (++name; *name; ++name) {
    if (!(*name == '_' || (*name >= 'A' && *name <= 'Z') || (*name >= '0' && *name <= '9')))
      return 0;
  }
  return 1;
}

int flowie_control_config_turbodb_secret_option(const char *keyword) {
  static const char *const secret_options[] = {
      "conninfo", "password", "sslpassword", "uri", "url"};
  size_t index;
  if (!keyword) return 0;
  for (index = 0u; index < sizeof(secret_options) / sizeof(secret_options[0]); ++index) {
    if (strcmp(keyword, secret_options[index]) == 0) return 1;
  }
  return 0;
}

static int control_config_route_valid(const char *value) {
  if (!value || value[0] != '/' || value[1] == '\0') return 0;
  for (; *value; ++value) {
    if (*value == '?' || *value == '#' || *value == '\\') return 0;
  }
  return 1;
}

static int control_config_parse_tls(const json_value_t *tls, flowie_control_config_t *config,
                                    flowie_control_config_error_t *error) {
  static const char *const keys[] = {"cert_file", "key_file", "key_password_ref", "client_auth",
                                     "client_ca_file"};
  char client_auth[16] = {0};
  int rc =
      control_config_object(tls, "$.listener.tls", keys, sizeof(keys) / sizeof(keys[0]), error);
  if (rc != TURBO_OK) return rc;
  rc = control_config_text(turbo_json_object_get(tls, "cert_file"), "$.listener.tls.cert_file",
                           config->listener.tls.cert_file, sizeof(config->listener.tls.cert_file),
                           1, error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(tls, "key_file"), "$.listener.tls.key_file",
                             config->listener.tls.key_file, sizeof(config->listener.tls.key_file),
                             1, error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(tls, "client_ca_file"),
                             "$.listener.tls.client_ca_file", config->listener.tls.client_ca_file,
                             sizeof(config->listener.tls.client_ca_file), 0, error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(tls, "key_password_ref"),
                             "$.listener.tls.key_password_ref",
                             config->listener.tls.key_password_ref,
                             sizeof(config->listener.tls.key_password_ref), 0, error);
  if (rc == TURBO_OK && config->listener.tls.key_password_ref[0] &&
      !flowie_control_config_secret_ref_valid(config->listener.tls.key_password_ref))
    rc = control_config_error(error, TURBO_EINVAL, "$.listener.tls.key_password_ref",
                              "only env:// secret references are accepted");
  if (rc == TURBO_OK)
    rc =
        control_config_text(turbo_json_object_get(tls, "client_auth"), "$.listener.tls.client_auth",
                            client_auth, sizeof(client_auth), 0, error);
  if (rc == TURBO_OK && (!client_auth[0] || strcmp(client_auth, "none") == 0))
    config->listener.tls.client_auth_required = 0;
  else if (rc == TURBO_OK && strcmp(client_auth, "required") == 0)
    config->listener.tls.client_auth_required = 1;
  else if (rc == TURBO_OK)
    rc = control_config_error(error, TURBO_EINVAL, "$.listener.tls.client_auth",
                              "expected none or required");
  if (rc == TURBO_OK && config->listener.tls.client_auth_required &&
      !config->listener.tls.client_ca_file[0])
    rc = control_config_error(error, TURBO_EINVAL, "$.listener.tls.client_ca_file",
                              "required when client_auth is required");
  if (rc == TURBO_OK && !config->listener.tls.client_auth_required &&
      config->listener.tls.client_ca_file[0])
    rc = control_config_error(error, TURBO_EINVAL, "$.listener.tls.client_ca_file",
                              "must be omitted when client_auth is none");
  return rc;
}

static int control_config_limit(const json_value_t *limits, const char *key, const char *path,
                                uint64_t minimum, uint64_t maximum, size_t *target,
                                flowie_control_config_error_t *error) {
  json_value_t *value = turbo_json_object_get(limits, key);
  uint64_t resolved;
  int rc;
  if (!value) return TURBO_OK;
  rc = control_config_integer(value, path, minimum, maximum, &resolved, error);
  if (rc == TURBO_OK) *target = (size_t)resolved;
  return rc;
}

static int control_config_parse_limits(const json_value_t *limits, flowie_control_config_t *config,
                                       flowie_control_config_error_t *error) {
  static const char *const keys[] = {
      "max_header_name_length", "max_header_value_length", "max_url_length",
      "max_cookie_name_length", "max_cookie_value_length", "max_json_depth",
      "max_log_message_length", "max_request_body_size",   "max_headers_count"};
  uint64_t count;
  int rc = control_config_object(limits, "$.listener.limits", keys, sizeof(keys) / sizeof(keys[0]),
                                 error);
  if (rc == TURBO_OK)
    rc = control_config_limit(limits, keys[0], "$.listener.limits.max_header_name_length", 32u,
                              1024u, &config->listener.limits.max_header_name_length, error);
  if (rc == TURBO_OK)
    rc = control_config_limit(limits, keys[1], "$.listener.limits.max_header_value_length", 256u,
                              16384u, &config->listener.limits.max_header_value_length, error);
  if (rc == TURBO_OK)
    rc = control_config_limit(limits, keys[2], "$.listener.limits.max_url_length", 256u, 8192u,
                              &config->listener.limits.max_url_length, error);
  if (rc == TURBO_OK)
    rc = control_config_limit(limits, keys[3], "$.listener.limits.max_cookie_name_length", 32u,
                              1024u, &config->listener.limits.max_cookie_name_length, error);
  if (rc == TURBO_OK)
    rc = control_config_limit(limits, keys[4], "$.listener.limits.max_cookie_value_length", 128u,
                              16384u, &config->listener.limits.max_cookie_value_length, error);
  if (rc == TURBO_OK)
    rc = control_config_limit(limits, keys[5], "$.listener.limits.max_json_depth", 4u, 128u,
                              &config->listener.limits.max_json_depth, error);
  if (rc == TURBO_OK)
    rc = control_config_limit(limits, keys[6], "$.listener.limits.max_log_message_length", 256u,
                              16384u, &config->listener.limits.max_log_message_length, error);
  if (rc == TURBO_OK)
    rc = control_config_limit(limits, keys[7], "$.listener.limits.max_request_body_size", 1024u,
                              1048576u, &config->listener.limits.max_request_body_size, error);
  if (rc == TURBO_OK && turbo_json_object_get(limits, keys[8])) {
    rc = control_config_integer(turbo_json_object_get(limits, keys[8]),
                                "$.listener.limits.max_headers_count", 8u, 256u, &count, error);
    if (rc == TURBO_OK) config->listener.limits.max_headers_count = (int)count;
  }
  return rc;
}

static int control_config_parse_listener(const json_value_t *listener,
                                         flowie_control_config_t *config,
                                         flowie_control_config_error_t *error) {
  static const char *const keys[] = {"host", "port", "coroutine_stack_size", "tls", "limits"};
  uint64_t port;
  int rc =
      control_config_object(listener, "$.listener", keys, sizeof(keys) / sizeof(keys[0]), error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(listener, "host"), "$.listener.host",
                             config->listener.host, sizeof(config->listener.host), 0, error);
  if (rc == TURBO_OK && !config->listener.host[0])
    memcpy(config->listener.host, "127.0.0.1", sizeof("127.0.0.1"));
  if (rc == TURBO_OK && turbo_json_object_get(listener, "port")) {
    rc = control_config_integer(turbo_json_object_get(listener, "port"), "$.listener.port", 1u,
                                65535u, &port, error);
    if (rc == TURBO_OK) config->listener.port = (uint16_t)port;
  }
  if (rc == TURBO_OK)
    rc = control_config_limit(listener, "coroutine_stack_size", "$.listener.coroutine_stack_size",
                              FLOWIE_CONTROL_CONFIG_LISTENER_MIN_COROUTINE_STACK_SIZE,
                              FLOWIE_CONTROL_CONFIG_LISTENER_MAX_COROUTINE_STACK_SIZE,
                              &config->listener.coroutine_stack_size, error);
  if (rc == TURBO_OK)
    rc = control_config_parse_tls(turbo_json_object_get(listener, "tls"), config, error);
  if (rc == TURBO_OK && turbo_json_object_get(listener, "limits"))
    rc = control_config_parse_limits(turbo_json_object_get(listener, "limits"), config, error);
  return rc;
}

static int control_config_parse_storage(const json_value_t *storage,
                                        flowie_control_config_t *config,
                                        flowie_control_config_error_t *error) {
  static const char *const storage_keys[] = {"turbodb"};
  static const char *const turbodb_keys[] = {"driver", "options"};
  json_value_t *turbodb;
  json_value_t *options;
  int rc = control_config_object(storage, "$.storage", storage_keys,
                                 sizeof(storage_keys) / sizeof(storage_keys[0]), error);
  turbodb = storage ? turbo_json_object_get(storage, "turbodb") : NULL;
  if (rc == TURBO_OK)
    rc = control_config_object(turbodb, "$.storage.turbodb", turbodb_keys,
                               sizeof(turbodb_keys) / sizeof(turbodb_keys[0]), error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(turbodb, "driver"), "$.storage.turbodb.driver",
                             config->turbodb.driver, sizeof(config->turbodb.driver), 1, error);
  options = turbodb ? turbo_json_object_get(turbodb, "options") : NULL;
  if (rc == TURBO_OK && options && turbo_json_type(options) != TURBO_JSON_OBJECT)
    rc = control_config_error(error, TURBO_EINVAL, "$.storage.turbodb.options", "expected mapping");
  if (rc == TURBO_OK && options &&
      turbo_json_object_size(options) > FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX)
    rc = control_config_error(error, TURBO_ENOSPC, "$.storage.turbodb.options",
                              "too many TurboDB options");
  for (size_t index = 0u; rc == TURBO_OK && options && index < turbo_json_object_size(options);
       ++index) {
    const char *key = turbo_json_object_key(options, index);
    char path[FLOWIE_CONTROL_CONFIG_ERROR_PATH_MAX + 1u];
    if (!key || !key[0]) {
      rc = control_config_error(error, TURBO_EINVAL, "$.storage.turbodb.options",
                                "empty option keyword");
      break;
    }
    (void)snprintf(path, sizeof(path), "$.storage.turbodb.options.%s", key);
    if (strlen(key) > FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_KEY_MAX) {
      rc = control_config_error(error, TURBO_ENAMETOOLONG, path, "option keyword exceeds limit");
      break;
    }
    rc = control_config_text(turbo_json_object_get(options, key), path,
                             config->turbodb.options[index].value,
                             sizeof(config->turbodb.options[index].value), 1, error);
    if (rc == TURBO_OK && flowie_control_config_turbodb_secret_option(key) &&
        !flowie_control_config_secret_ref_valid(config->turbodb.options[index].value))
      rc = control_config_error(error, TURBO_EINVAL, path,
                                "sensitive database options require a valid env:// reference");
    if (rc == TURBO_OK) {
      (void)snprintf(config->turbodb.options[index].keyword,
                     sizeof(config->turbodb.options[index].keyword), "%s", key);
      config->turbodb.option_count = index + 1u;
    }
  }
  return rc;
}

static int control_config_parse_management(const json_value_t *management,
                                           flowie_control_config_t *config,
                                           flowie_control_config_error_t *error) {
  static const char *const keys[] = {"rpc_path", "rpc_max_request_size", "session",
                                     "login_executor"};
  static const char *const session_keys[] = {"capacity", "max_sessions_per_principal",
                                             "ttl_seconds"};
  static const char *const executor_keys[] = {"workers", "queue_capacity", "deadline_ms"};
  json_value_t *session;
  json_value_t *executor;
  uint64_t request_size;
  uint64_t value;
  int rc = control_config_object(management, "$.management", keys, sizeof(keys) / sizeof(keys[0]),
                                 error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(management, "rpc_path"), "$.management.rpc_path",
                             config->management.rpc_path, sizeof(config->management.rpc_path), 0,
                             error);
  if (rc == TURBO_OK && !config->management.rpc_path[0])
    memcpy(config->management.rpc_path, "/v2/control/rpc", sizeof("/v2/control/rpc"));
  if (rc == TURBO_OK && !control_config_route_valid(config->management.rpc_path))
    rc = control_config_error(error, TURBO_EINVAL, "$.management.rpc_path",
                              "expected one static absolute route");
  if (rc == TURBO_OK && turbo_json_object_get(management, "rpc_max_request_size")) {
    rc = control_config_integer(turbo_json_object_get(management, "rpc_max_request_size"),
                                "$.management.rpc_max_request_size", 1024u, 65536u, &request_size,
                                error);
    if (rc == TURBO_OK) config->management.rpc_max_request_size = (size_t)request_size;
  }
  session = turbo_json_object_get(management, "session");
  if (rc == TURBO_OK && session)
    rc = control_config_object(session, "$.management.session", session_keys,
                               sizeof(session_keys) / sizeof(session_keys[0]), error);
  if (rc == TURBO_OK && session && turbo_json_object_get(session, "capacity")) {
    rc = control_config_integer(turbo_json_object_get(session, "capacity"),
                                "$.management.session.capacity", 1u,
                                FLOWIE_CONTROL_CONFIG_SESSION_MAX_CAPACITY, &value, error);
    if (rc == TURBO_OK) config->management.session_capacity = (size_t)value;
  }
  if (rc == TURBO_OK && session && turbo_json_object_get(session, "max_sessions_per_principal")) {
    rc = control_config_integer(turbo_json_object_get(session, "max_sessions_per_principal"),
                                "$.management.session.max_sessions_per_principal", 1u,
                                FLOWIE_CONTROL_CONFIG_SESSION_MAX_PER_PRINCIPAL, &value, error);
    if (rc == TURBO_OK) config->management.session_max_sessions_per_principal = (size_t)value;
  }
  if (rc == TURBO_OK && session && turbo_json_object_get(session, "ttl_seconds")) {
    rc = control_config_integer(turbo_json_object_get(session, "ttl_seconds"),
                                "$.management.session.ttl_seconds", 60u,
                                FLOWIE_CONTROL_CONFIG_SESSION_MAX_TTL_SECONDS, &value, error);
    if (rc == TURBO_OK) config->management.session_ttl_seconds = value;
  }
  executor = turbo_json_object_get(management, "login_executor");
  if (rc == TURBO_OK && executor) {
    rc = control_config_object(executor, "$.management.login_executor", executor_keys,
                               sizeof(executor_keys) / sizeof(executor_keys[0]), error);
    if (rc == TURBO_OK) config->management.login_executor_configured = 1;
  }
  if (rc == TURBO_OK && executor && turbo_json_object_get(executor, executor_keys[0])) {
    rc = control_config_integer(
        turbo_json_object_get(executor, executor_keys[0]), "$.management.login_executor.workers",
        1u, FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_WORKERS, &value, error);
    if (rc == TURBO_OK) config->management.login_executor_workers = (uint32_t)value;
  }
  if (rc == TURBO_OK && executor && turbo_json_object_get(executor, executor_keys[1])) {
    rc = control_config_integer(turbo_json_object_get(executor, executor_keys[1]),
                                "$.management.login_executor.queue_capacity", 1u,
                                FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_QUEUE_CAPACITY,
                                &value, error);
    if (rc == TURBO_OK) config->management.login_executor_queue_capacity = (size_t)value;
  }
  if (rc == TURBO_OK && executor && turbo_json_object_get(executor, executor_keys[2])) {
    rc = control_config_integer(turbo_json_object_get(executor, executor_keys[2]),
                                "$.management.login_executor.deadline_ms", 1u,
                                FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_DEADLINE_MS, &value,
                                error);
    if (rc == TURBO_OK) config->management.login_executor_deadline_ms = (uint32_t)value;
  }
  return rc;
}

static int control_config_parse_external_https_tls(const json_value_t *tls,
                                                   flowie_control_config_t *config,
                                                   flowie_control_config_error_t *error) {
  static const char *const keys[] = {"ca_file", "client_cert_file", "client_key_file",
                                     "client_key_password_ref"};
  flowie_control_config_external_https_tls_t *resolved = &config->auth.external_https.tls;
  json_value_t *value;
  int rc;
  if (!tls) return TURBO_OK;
  rc = control_config_object(tls, "$.auth.external_https.tls", keys, sizeof(keys) / sizeof(keys[0]),
                             error);
  value = turbo_json_object_get(tls, keys[0]);
  if (rc == TURBO_OK)
    rc = control_config_text(value, "$.auth.external_https.tls.ca_file", resolved->ca_file,
                             sizeof(resolved->ca_file), value != NULL, error);
  value = turbo_json_object_get(tls, keys[1]);
  if (rc == TURBO_OK)
    rc = control_config_text(value, "$.auth.external_https.tls.client_cert_file",
                             resolved->client_cert_file, sizeof(resolved->client_cert_file),
                             value != NULL, error);
  value = turbo_json_object_get(tls, keys[2]);
  if (rc == TURBO_OK)
    rc = control_config_text(value, "$.auth.external_https.tls.client_key_file",
                             resolved->client_key_file, sizeof(resolved->client_key_file),
                             value != NULL, error);
  value = turbo_json_object_get(tls, keys[3]);
  if (rc == TURBO_OK)
    rc = control_config_text(value, "$.auth.external_https.tls.client_key_password_ref",
                             resolved->client_key_password_ref,
                             sizeof(resolved->client_key_password_ref), value != NULL, error);
  if (rc == TURBO_OK && (!!resolved->client_cert_file[0] != !!resolved->client_key_file[0]))
    rc = control_config_error(error, TURBO_EINVAL, "$.auth.external_https.tls",
                              "client_cert_file and client_key_file must be configured together");
  if (rc == TURBO_OK && resolved->client_key_password_ref[0] && !resolved->client_key_file[0])
    rc = control_config_error(error, TURBO_EINVAL,
                              "$.auth.external_https.tls.client_key_password_ref",
                              "client key password requires a client identity");
  if (rc == TURBO_OK && resolved->client_key_password_ref[0] &&
      !flowie_control_config_secret_ref_valid(resolved->client_key_password_ref))
    rc = control_config_error(error, TURBO_EINVAL,
                              "$.auth.external_https.tls.client_key_password_ref",
                              "only env:// secret references are accepted");
  return rc;
}

static int control_config_parse_external_https(const json_value_t *external,
                                               flowie_control_config_t *config,
                                               flowie_control_config_error_t *error) {
  static const char *const keys[] = {
      "url",        "service_token_ref", "trusted_issuer", "subject_type",
      "timeout_ms", "max_response_size", "max_in_flight",  "tls"};
  flowie_control_config_external_https_t *resolved = &config->auth.external_https;
  uint64_t number;
  int rc = control_config_object(external, "$.auth.external_https", keys,
                                 sizeof(keys) / sizeof(keys[0]), error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(external, keys[0]), "$.auth.external_https.url",
                             resolved->url, sizeof(resolved->url), 1, error);
  if (rc == TURBO_OK && strncmp(resolved->url, "https://", sizeof("https://") - 1u) != 0)
    rc = control_config_error(error, TURBO_EINVAL, "$.auth.external_https.url",
                              "only HTTPS URLs are accepted");
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(external, keys[1]),
                             "$.auth.external_https.service_token_ref", resolved->service_token_ref,
                             sizeof(resolved->service_token_ref), 1, error);
  if (rc == TURBO_OK && !flowie_control_config_secret_ref_valid(resolved->service_token_ref))
    rc = control_config_error(error, TURBO_EINVAL, "$.auth.external_https.service_token_ref",
                              "only env:// secret references are accepted");
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(external, keys[2]),
                             "$.auth.external_https.trusted_issuer", resolved->trusted_issuer,
                             sizeof(resolved->trusted_issuer), 1, error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(external, keys[3]),
                             "$.auth.external_https.subject_type", resolved->subject_type,
                             sizeof(resolved->subject_type), 1, error);
  if (rc == TURBO_OK && turbo_json_object_get(external, keys[4])) {
    rc = control_config_integer(
        turbo_json_object_get(external, keys[4]), "$.auth.external_https.timeout_ms", 1u,
        FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_MAX_TIMEOUT_MS, &number, error);
    if (rc == TURBO_OK) resolved->timeout_ms = (uint32_t)number;
  }
  if (rc == TURBO_OK && turbo_json_object_get(external, keys[5])) {
    rc = control_config_integer(
        turbo_json_object_get(external, keys[5]), "$.auth.external_https.max_response_size",
        FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_MIN_RESPONSE_SIZE,
        FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_MAX_RESPONSE_SIZE, &number, error);
    if (rc == TURBO_OK) resolved->max_response_size = (size_t)number;
  }
  if (rc == TURBO_OK && turbo_json_object_get(external, keys[6])) {
    rc = control_config_integer(turbo_json_object_get(external, keys[6]),
                                "$.auth.external_https.max_in_flight", 1u,
                                FLOWIE_CONTROL_CONFIG_EXTERNAL_HTTPS_MAX_IN_FLIGHT, &number, error);
    if (rc == TURBO_OK) resolved->max_in_flight = (uint32_t)number;
  }
  if (rc == TURBO_OK)
    rc = control_config_parse_external_https_tls(turbo_json_object_get(external, keys[7]), config,
                                                 error);
  if (rc == TURBO_OK) resolved->enabled = 1;
  return rc;
}

static int control_config_parse_auth_local_executor(const json_value_t *executor,
                                                    flowie_control_config_t *config,
                                                    flowie_control_config_error_t *error) {
  static const char *const keys[] = {"workers", "queue_capacity", "deadline_ms"};
  flowie_control_config_auth_local_executor_t *resolved = &config->auth.local_executor;
  uint64_t number;
  int rc;
  if (!executor) return TURBO_OK;
  rc = control_config_object(executor, "$.auth.local_executor", keys,
                             sizeof(keys) / sizeof(keys[0]), error);
  if (rc == TURBO_OK && turbo_json_object_get(executor, keys[0])) {
    rc = control_config_integer(
        turbo_json_object_get(executor, keys[0]), "$.auth.local_executor.workers", 1u,
        FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_WORKERS, &number, error);
    if (rc == TURBO_OK) resolved->workers = (uint32_t)number;
  }
  if (rc == TURBO_OK && turbo_json_object_get(executor, keys[1])) {
    rc = control_config_integer(
        turbo_json_object_get(executor, keys[1]), "$.auth.local_executor.queue_capacity", 1u,
        FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_QUEUE_CAPACITY, &number, error);
    if (rc == TURBO_OK) resolved->queue_capacity = (size_t)number;
  }
  if (rc == TURBO_OK && turbo_json_object_get(executor, keys[2])) {
    rc = control_config_integer(
        turbo_json_object_get(executor, keys[2]), "$.auth.local_executor.deadline_ms", 1u,
        FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_MAX_DEADLINE_MS, &number, error);
    if (rc == TURBO_OK) resolved->deadline_ms = (uint32_t)number;
  }
  if (rc == TURBO_OK) resolved->configured = 1;
  return rc;
}

static int control_config_parse_auth(const json_value_t *auth, flowie_control_config_t *config,
                                     flowie_control_config_error_t *error) {
  static const char *const keys[] = {"enabled",
                                     "listener_id",
                                     "method",
                                     "principal_ttl_seconds",
                                     "credential_cache_capacity",
                                     "credential_cache_ttl_seconds",
                                     "local_executor",
                                     "external_https"};
  json_value_t *external;
  uint64_t number;
  int rc = control_config_object(auth, "$.auth", keys, sizeof(keys) / sizeof(keys[0]), error);
  if (rc == TURBO_OK && turbo_json_object_get(auth, "enabled"))
    rc = control_config_boolean(turbo_json_object_get(auth, "enabled"), "$.auth.enabled",
                                &config->auth.enabled, error);
  if (rc != TURBO_OK) return rc;
  if (!config->auth.enabled) {
    if (turbo_json_object_get(auth, "local_executor"))
      return control_config_error(error, TURBO_EINVAL, "$.auth.local_executor",
                                  "local executor requires auth.enabled");
    if (turbo_json_object_get(auth, "external_https"))
      return control_config_error(error, TURBO_EINVAL, "$.auth.external_https",
                                  "external HTTPS authentication requires auth.enabled");
    return TURBO_OK;
  }
  rc = control_config_text(turbo_json_object_get(auth, "listener_id"), "$.auth.listener_id",
                           config->auth.listener_id, sizeof(config->auth.listener_id), 1, error);
  if (rc == TURBO_OK)
    rc = control_config_text(turbo_json_object_get(auth, "method"), "$.auth.method",
                             config->auth.method, sizeof(config->auth.method), 1, error);
  if (rc == TURBO_OK && turbo_json_object_get(auth, "principal_ttl_seconds")) {
    rc = control_config_integer(turbo_json_object_get(auth, "principal_ttl_seconds"),
                                "$.auth.principal_ttl_seconds", 1u,
                                FLOWIE_CONTROL_AUTH_MAX_PRINCIPAL_TTL_SECONDS, &number, error);
    if (rc == TURBO_OK) config->auth.principal_ttl_seconds = number;
  }
  if (rc == TURBO_OK && turbo_json_object_get(auth, "credential_cache_capacity")) {
    rc = control_config_integer(turbo_json_object_get(auth, "credential_cache_capacity"),
                                "$.auth.credential_cache_capacity", 1u,
                                FLOWIE_CONTROL_CONFIG_AUTH_CACHE_CAPACITY_MAX, &number, error);
    if (rc == TURBO_OK) config->auth.credential_cache_capacity = (size_t)number;
  }
  if (rc == TURBO_OK && turbo_json_object_get(auth, "credential_cache_ttl_seconds")) {
    rc = control_config_integer(turbo_json_object_get(auth, "credential_cache_ttl_seconds"),
                                "$.auth.credential_cache_ttl_seconds", 1u,
                                FLOWIE_CONTROL_CONFIG_AUTH_CACHE_TTL_SECONDS_MAX, &number, error);
    if (rc == TURBO_OK) config->auth.credential_cache_ttl_seconds = number;
  }
  if (rc == TURBO_OK)
    rc = control_config_parse_auth_local_executor(turbo_json_object_get(auth, "local_executor"),
                                                  config, error);
  external = turbo_json_object_get(auth, "external_https");
  if (rc == TURBO_OK && external && config->auth.local_executor.configured)
    rc = control_config_error(error, TURBO_EINVAL, "$.auth.local_executor",
                              "local executor cannot be configured with external HTTPS auth");
  if (rc == TURBO_OK && external) rc = control_config_parse_external_https(external, config, error);
  return rc;
}

int flowie_control_config_parse_yaml(const char *yaml, size_t yaml_size,
                                     flowie_control_config_t *out,
                                     flowie_control_config_error_t *error) {
  static const char *const root_keys[] = {"version",    "listener",  "storage",
                                          "management", "dashboard", "auth"};
  static const char *const dashboard_keys[] = {"enabled"};
  flowie_control_config_t resolved = FLOWIE_CONTROL_CONFIG_INIT;
  turbo_yaml_doc_t *yaml_document = NULL;
  json_value_t *document = NULL;
  json_value_t *version;
  json_value_t *dashboard;
  uint64_t version_number = 0u;
  int rc;
  if (!yaml || yaml_size == 0u || !out || out->size < sizeof(*out) || !error ||
      error->size < sizeof(*error))
    return TURBO_EINVAL;
  *out = resolved;
  *error = (flowie_control_config_error_t)FLOWIE_CONTROL_CONFIG_ERROR_INIT;
  if (turbo_parse_yaml((const uint8_t *)yaml, yaml_size, &yaml_document) != TURBO_OK ||
      !yaml_document)
    return control_config_error(error, TURBO_EINVAL, "$", "invalid YAML document");
  document = turbo_yaml_to_json(yaml_document);
  if (!document) {
    rc = control_config_error(error, TURBO_EINVAL, "$", "YAML cannot be represented as JSON");
    goto done;
  }
  rc = control_config_object(document, "$", root_keys, sizeof(root_keys) / sizeof(root_keys[0]),
                             error);
  version = turbo_json_object_get(document, "version");
  if (rc == TURBO_OK)
    rc = control_config_integer(version, "$.version", FLOWIE_CONTROL_CONFIG_VERSION,
                                FLOWIE_CONTROL_CONFIG_VERSION, &version_number, error);
  if (rc == TURBO_OK) resolved.version = (uint32_t)version_number;
  if (rc == TURBO_OK)
    rc = control_config_parse_listener(turbo_json_object_get(document, "listener"), &resolved,
                                       error);
  if (rc == TURBO_OK)
    rc = control_config_parse_storage(turbo_json_object_get(document, "storage"), &resolved, error);
  if (rc == TURBO_OK)
    rc = control_config_parse_management(turbo_json_object_get(document, "management"), &resolved,
                                         error);
  dashboard = turbo_json_object_get(document, "dashboard");
  if (rc == TURBO_OK && dashboard) {
    rc = control_config_object(dashboard, "$.dashboard", dashboard_keys,
                               sizeof(dashboard_keys) / sizeof(dashboard_keys[0]), error);
    if (rc == TURBO_OK && turbo_json_object_get(dashboard, "enabled"))
      rc = control_config_boolean(turbo_json_object_get(dashboard, "enabled"),
                                  "$.dashboard.enabled", &resolved.dashboard_enabled, error);
  }
  if (rc == TURBO_OK && turbo_json_object_get(document, "auth"))
    rc = control_config_parse_auth(turbo_json_object_get(document, "auth"), &resolved, error);
  if (rc == TURBO_OK && resolved.auth.external_https.enabled &&
      resolved.management.login_executor_configured)
    rc = control_config_error(error, TURBO_EINVAL, "$.management.login_executor",
                              "login executor is only valid for local authentication");
  if (rc == TURBO_OK &&
      resolved.listener.limits.max_request_body_size < resolved.management.rpc_max_request_size)
    rc = control_config_error(error, TURBO_ERANGE, "$.listener.limits.max_request_body_size",
                              "must cover management rpc_max_request_size");
  if (rc == TURBO_OK) *out = resolved;

done:
  if (document) turbo_free_json((turbo_json_doc_t **)&document);
  turbo_free_yaml(&yaml_document);
  if (rc != TURBO_OK) *out = (flowie_control_config_t)FLOWIE_CONTROL_CONFIG_INIT;
  return rc;
}

int flowie_control_config_load(const char *path, flowie_control_config_t *out,
                               flowie_control_config_error_t *error) {
  turbo_fs_buf_t buffer = {0};
  int rc;
  if (!path || !path[0] || !out || out->size < sizeof(*out) || !error ||
      error->size < sizeof(*error))
    return TURBO_EINVAL;
  *out = (flowie_control_config_t)FLOWIE_CONTROL_CONFIG_INIT;
  *error = (flowie_control_config_error_t)FLOWIE_CONTROL_CONFIG_ERROR_INIT;
  rc = turbo_fs_read_file(path, &buffer);
  if (rc != TURBO_OK)
    return control_config_error(error, rc, "$", "cannot read controller configuration file");
  rc = flowie_control_config_parse_yaml(buffer.base, buffer.len, out, error);
  turbo_fs_buf_free(&buffer);
  return rc;
}
