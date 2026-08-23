#include "flowie_security_sqlite.h"

#include "orm.h"
#include "turbo_error.h"
#include "turbo_str.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct flowie_security_sqlite_loaded_s {
  flowie_security_rule_t *rules;
} flowie_security_sqlite_loaded_t;

struct flowie_security_sqlite_provider_s {
  tstr database_path;
  tstr namespace_name;
  int busy_timeout_ms;
  size_t max_rules;
  flowie_security_policy_provider_t interface;
};

static int flowie_security_orm_status(orm_status_t status) {
  switch (status) {
  case ORM_STATUS_OK: return TURBO_OK;
  case ORM_STATUS_INVALID_ARGUMENT:
  case ORM_STATUS_ABI_MISMATCH:
  case ORM_STATUS_TYPE_ERROR:
  case ORM_STATUS_OUT_OF_RANGE:
  case ORM_STATUS_NULL_VALUE:
  case ORM_STATUS_INVALID_STATE: return TURBO_EINVAL;
  case ORM_STATUS_OUT_OF_MEMORY: return TURBO_ENOMEM;
  case ORM_STATUS_LIMIT_EXCEEDED: return TURBO_ENOSPC;
  case ORM_STATUS_BUSY: return TURBO_EBUSY;
  case ORM_STATUS_UNSUPPORTED: return TURBO_ENOTSUP;
  default: return TURBO_EIO;
  }
}

static orm_value_t flowie_security_orm_text(const char *value) {
  orm_value_t out;
  memset(&out, 0, sizeof(out));
  out.kind = ORM_VALUE_TEXT;
  out.data.text_value = orm_view(value);
  return out;
}

static int flowie_security_orm_query(orm_connection_t *connection,
                                     orm_transaction_t *transaction, const char *sql,
                                     const char *parameter, orm_result_t **out) {
  orm_error_t error;
  orm_query_t *query = NULL;
  orm_status_t status;
  if (out) *out = NULL;
  if (!connection || !transaction || !sql || !out) return TURBO_EINVAL;
  orm_error_init(&error);
  status = orm_raw(connection, orm_view(sql), &query, &error);
  if (status == ORM_STATUS_OK && parameter)
    status = orm_query_bind(query, flowie_security_orm_text(parameter), &error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute_in_transaction(query, transaction, out, &error);
  orm_query_destroy(query);
  return flowie_security_orm_status(status);
}

static int flowie_security_orm_open(const flowie_security_sqlite_provider_t *provider,
                                    orm_connection_t **out) {
  orm_config_t config;
  orm_option_t options[2];
  orm_error_t error;
  char timeout[16];
  if (out) *out = NULL;
  if (!provider || !provider->database_path || !out) return TURBO_EINVAL;
  orm_config(&config);
  if (snprintf(timeout, sizeof(timeout), "%d", provider->busy_timeout_ms) <= 0)
    return TURBO_EINVAL;
  options[0].keyword = orm_view("filename");
  options[0].value = orm_view(provider->database_path);
  options[1].keyword = orm_view("busy_timeout_ms");
  options[1].value = orm_view(timeout);
  config.driver = orm_view("sqlite");
  config.options = options;
  config.option_count = 2u;
  config.max_result_rows = provider->max_rules + 1u;
  config.max_parameter_bytes = FLOWIE_SECURITY_ID_MAX;
  orm_error_init(&error);
  return flowie_security_orm_status(orm_connect(&config, out, &error));
}

static void flowie_security_sqlite_loaded_destroy(flowie_security_sqlite_loaded_t *loaded) {
  if (!loaded) return;
  free(loaded->rules);
  free(loaded);
}

static int flowie_security_sqlite_load(void *ctx, uint64_t required_version,
                                       flowie_security_policy_bundle_t *bundle_out) {
  static const char metadata_sql[] =
      "select policy_version,expires_at from turbo_flow_acl_bundle_v3 where namespace_name=?";
  static const char rules_sql[] =
      "select ordinal,rule_line from turbo_flow_acl_rule_v3 where namespace_name=? order by ordinal";
  flowie_security_sqlite_provider_t *provider = (flowie_security_sqlite_provider_t *)ctx;
  flowie_security_sqlite_loaded_t *loaded = NULL;
  orm_connection_t *connection = NULL;
  orm_transaction_t *transaction = NULL;
  orm_result_t *metadata = NULL;
  orm_result_t *rules = NULL;
  orm_error_t error;
  uint64_t metadata_rows = 0u;
  uint64_t rule_rows = 0u;
  uint64_t policy_version = 0u;
  uint64_t expires_at = 0u;
  int rc;
  if (!provider || !bundle_out || bundle_out->size < sizeof(*bundle_out)) return TURBO_EINVAL;
  *bundle_out = (flowie_security_policy_bundle_t)FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
  rc = flowie_security_orm_open(provider, &connection);
  orm_error_init(&error);
  if (rc == TURBO_OK)
    rc = flowie_security_orm_status(orm_transaction_begin(
        connection, ORM_ISOLATION_SERIALIZABLE, &transaction, &error));
  if (rc == TURBO_OK)
    rc = flowie_security_orm_query(connection, transaction, metadata_sql,
                                   provider->namespace_name, &metadata);
  if (rc == TURBO_OK)
    rc = flowie_security_orm_status(orm_result_row_count(metadata, &metadata_rows, &error));
  if (rc == TURBO_OK && metadata_rows == 0u) rc = TURBO_ENOENT;
  if (rc == TURBO_OK && metadata_rows != 1u) rc = TURBO_EPROTO;
  if (rc == TURBO_OK)
    rc = flowie_security_orm_status(
        orm_result_get_uint64(metadata, 0u, 0u, &policy_version, &error));
  if (rc == TURBO_OK)
    rc = flowie_security_orm_status(orm_result_get_uint64(metadata, 0u, 1u, &expires_at, &error));
  if (rc == TURBO_OK && (policy_version == 0u ||
                         (required_version != 0u && required_version != policy_version)))
    rc = TURBO_ENOENT;
  if (rc == TURBO_OK)
    rc = flowie_security_orm_query(connection, transaction, rules_sql,
                                   provider->namespace_name, &rules);
  if (rc == TURBO_OK)
    rc = flowie_security_orm_status(orm_result_row_count(rules, &rule_rows, &error));
  if (rc == TURBO_OK && (rule_rows == 0u || rule_rows > provider->max_rules))
    rc = rule_rows > provider->max_rules ? TURBO_ENOSPC : TURBO_EPROTO;
  if (rc == TURBO_OK) {
    loaded = (flowie_security_sqlite_loaded_t *)calloc(1u, sizeof(*loaded));
    if (!loaded) rc = TURBO_ENOMEM;
  }
  if (rc == TURBO_OK) {
    loaded->rules = (flowie_security_rule_t *)calloc((size_t)rule_rows, sizeof(*loaded->rules));
    if (!loaded->rules) rc = TURBO_ENOMEM;
  }
  for (uint64_t i = 0u; rc == TURBO_OK && i < rule_rows; ++i) {
    uint64_t ordinal = 0u;
    orm_string_view_t line;
    flowie_security_rule_t rule = FLOWIE_SECURITY_RULE_INIT;
    memset(&line, 0, sizeof(line));
    rc = flowie_security_orm_status(orm_result_get_uint64(rules, i, 0u, &ordinal, &error));
    if (rc == TURBO_OK)
      rc = flowie_security_orm_status(orm_result_get_text(rules, i, 1u, &line, &error));
    if (rc == TURBO_OK &&
        (ordinal != i || !line.data || line.len == 0u ||
         line.len > FLOWIE_SECURITY_RULE_LINE_MAX || memchr(line.data, '\0', line.len)))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK &&
        (flowie_security_rule_parse_line(line.data, line.len, &rule) != TURBO_OK ||
         strcmp(rule.domain_id, provider->namespace_name) != 0))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) loaded->rules[i] = rule;
  }
  if (rc == TURBO_OK)
    rc = flowie_security_orm_status(orm_transaction_commit(transaction, &error));
  else if (transaction)
    (void)orm_transaction_rollback(transaction, &error);
  if (rc == TURBO_OK) {
    bundle_out->policy_version = policy_version;
    bundle_out->expires_at = expires_at;
    bundle_out->rules = loaded->rules;
    bundle_out->rule_count = (size_t)rule_rows;
    bundle_out->provider_bundle = loaded;
    loaded = NULL;
  }
  orm_result_destroy(rules);
  orm_result_destroy(metadata);
  orm_transaction_destroy(transaction);
  orm_disconnect(connection);
  flowie_security_sqlite_loaded_destroy(loaded);
  return rc;
}

static void flowie_security_sqlite_release(void *ctx,
                                           flowie_security_policy_bundle_t *bundle) {
  (void)ctx;
  if (!bundle) return;
  flowie_security_sqlite_loaded_destroy(
      (flowie_security_sqlite_loaded_t *)bundle->provider_bundle);
  *bundle = (flowie_security_policy_bundle_t)FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
}

int flowie_security_sqlite_provider_create(const flowie_security_sqlite_config_t *config,
                                           flowie_security_sqlite_provider_t **out) {
  flowie_security_sqlite_provider_t *provider;
  orm_connection_t *probe = NULL;
  int rc;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) ||
      config->api_version != FLOWIE_SECURITY_SQLITE_API_VERSION || !config->database_path ||
      !config->database_path[0] || !config->namespace_name || !config->namespace_name[0] ||
      config->busy_timeout_ms < 0 || config->max_rules == 0u || !out)
    return TURBO_EINVAL;
  provider = (flowie_security_sqlite_provider_t *)calloc(1u, sizeof(*provider));
  if (!provider) return TURBO_ENOMEM;
  provider->database_path = tstr_dup(config->database_path);
  provider->namespace_name = tstr_dup(config->namespace_name);
  provider->busy_timeout_ms = config->busy_timeout_ms;
  provider->max_rules = config->max_rules;
  if (!provider->database_path || !provider->namespace_name) {
    flowie_security_sqlite_provider_destroy(provider);
    return TURBO_ENOMEM;
  }
  rc = flowie_security_orm_open(provider, &probe);
  orm_disconnect(probe);
  if (rc != TURBO_OK) {
    flowie_security_sqlite_provider_destroy(provider);
    return rc;
  }
  provider->interface = (flowie_security_policy_provider_t)FLOWIE_SECURITY_POLICY_PROVIDER_INIT;
  provider->interface.ctx = provider;
  provider->interface.load = flowie_security_sqlite_load;
  provider->interface.release = flowie_security_sqlite_release;
  *out = provider;
  return TURBO_OK;
}

const flowie_security_policy_provider_t *flowie_security_sqlite_provider_interface(
    const flowie_security_sqlite_provider_t *provider) {
  return provider ? &provider->interface : NULL;
}

void flowie_security_sqlite_provider_destroy(flowie_security_sqlite_provider_t *provider) {
  if (!provider) return;
  tstr_freep(&provider->database_path);
  tstr_freep(&provider->namespace_name);
  free(provider);
}
