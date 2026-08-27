#include "flowie_security_sqlite.h"
#include "flowie_orm_flow_internal.h"

#include "orm.h"
#include "turbo_error.h"
#include "turbo_str.h"

#include <turbostl/vec.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct flowie_security_sqlite_loaded_s {
  vec_t rules;
} flowie_security_sqlite_loaded_t;

struct flowie_security_sqlite_provider_s {
  tstr database_path;
  tstr namespace_name;
  int busy_timeout_ms;
  size_t max_rules;
  flowie_security_policy_provider_t interface;
};

static int flowie_security_orm_status(orm_status_t status) {
  return flowie_orm_status_to_turbo(status);
}

static int flowie_security_stl_status(stl_status status) {
  switch (status) {
  case STL_OK: return TURBO_OK;
  case STL_OUT_OF_MEMORY: return TURBO_ENOMEM;
  case STL_CAPACITY_EXCEEDED: return TURBO_ENOSPC;
  case STL_INVALID_ARGUMENT: return TURBO_EINVAL;
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

static int flowie_security_orm_query_visit(
    orm_connection_t *connection, orm_transaction_t *transaction, const char *sql,
    const char *parameter, const flowie_orm_column_t *columns, size_t column_count,
    size_t max_rows, flowie_orm_row_visit_fn visit, void *visit_ctx, size_t *row_count) {
  orm_error_t error;
  orm_query_t *query = NULL;
  orm_status_t status;
  int rc;
  if (!connection || !transaction || !sql || !columns || !visit) return TURBO_EINVAL;
  orm_error_init(&error);
  status = orm_raw(connection, orm_view(sql), &query, &error);
  if (status == ORM_STATUS_OK && parameter)
    status = orm_query_bind(query, flowie_security_orm_text(parameter), &error);
  rc = flowie_security_orm_status(status);
  if (rc == TURBO_OK)
    rc = flowie_orm_query_visit(query, transaction, columns, column_count, max_rows, 0u,
                                visit, visit_ctx, row_count);
  orm_query_destroy(query);
  return rc;
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
  vec_destroy(&loaded->rules);
  free(loaded);
}

typedef struct flowie_security_metadata_load_s {
  uint64_t policy_version;
  uint64_t expires_at;
} flowie_security_metadata_load_t;

static int flowie_security_metadata_visit(void *ctx, const flowie_orm_row_t *row,
                                          size_t row_index) {
  flowie_security_metadata_load_t *metadata = (flowie_security_metadata_load_t *)ctx;
  if (!metadata || row_index != 0u) return TURBO_EPROTO;
  metadata->policy_version = flowie_orm_row_uint64(row, 0u);
  metadata->expires_at = flowie_orm_row_uint64(row, 1u);
  return TURBO_OK;
}

typedef struct flowie_security_rules_load_s {
  const flowie_security_sqlite_provider_t *provider;
  vec_t *rules;
} flowie_security_rules_load_t;

static int flowie_security_rule_visit(void *ctx, const flowie_orm_row_t *row,
                                      size_t row_index) {
  flowie_security_rules_load_t *load = (flowie_security_rules_load_t *)ctx;
  const uint64_t ordinal = flowie_orm_row_uint64(row, 0u);
  const tstr line = flowie_orm_row_buffer(row, 1u);
  const size_t line_size = tstr_len(line);
  flowie_security_rule_t rule = FLOWIE_SECURITY_RULE_INIT;
  if (!load || !load->provider || !load->rules || row_index != vec_size(load->rules) ||
      ordinal != row_index || !line ||
      line_size == 0u || line_size > FLOWIE_SECURITY_RULE_LINE_MAX ||
      memchr(line, '\0', line_size))
    return TURBO_EPROTO;
  if (flowie_security_rule_parse_line(line, line_size, &rule) != TURBO_OK ||
      strcmp(rule.domain_id, load->provider->namespace_name) != 0)
    return TURBO_EPROTO;
  return flowie_security_stl_status(vec_push(load->rules, &rule));
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
  orm_error_t error;
  flowie_security_metadata_load_t metadata = {0};
  flowie_security_rules_load_t rules_load;
  size_t metadata_rows = 0u;
  size_t rule_rows = 0u;
  int rc;
  static const flowie_orm_column_t metadata_columns[] = {
      {"policy_version", FLOWIE_ORM_COLUMN_UINT64},
      {"expires_at", FLOWIE_ORM_COLUMN_UINT64},
  };
  static const flowie_orm_column_t rule_columns[] = {
      {"ordinal", FLOWIE_ORM_COLUMN_UINT64},
      {"rule_line", FLOWIE_ORM_COLUMN_TEXT},
  };
  if (!provider || !bundle_out || bundle_out->size < sizeof(*bundle_out)) return TURBO_EINVAL;
  *bundle_out = (flowie_security_policy_bundle_t)FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
  rc = flowie_security_orm_open(provider, &connection);
  orm_error_init(&error);
  if (rc == TURBO_OK)
    rc = flowie_security_orm_status(orm_transaction_begin(
        connection, ORM_ISOLATION_SERIALIZABLE, &transaction, &error));
  if (rc == TURBO_OK)
    rc = flowie_security_orm_query_visit(
        connection, transaction, metadata_sql, provider->namespace_name, metadata_columns,
        sizeof(metadata_columns) / sizeof(metadata_columns[0]), 2u,
        flowie_security_metadata_visit, &metadata, &metadata_rows);
  if (rc == TURBO_OK && metadata_rows == 0u) rc = TURBO_ENOENT;
  if (rc == TURBO_OK && metadata_rows != 1u) rc = TURBO_EPROTO;
  if (rc == TURBO_OK &&
      (metadata.policy_version == 0u ||
       (required_version != 0u && required_version != metadata.policy_version)))
    rc = TURBO_ENOENT;
  if (rc == TURBO_OK) {
    loaded = (flowie_security_sqlite_loaded_t *)calloc(1u, sizeof(*loaded));
    if (!loaded) rc = TURBO_ENOMEM;
  }
  if (rc == TURBO_OK) {
    rc = flowie_security_stl_status(vec_init_bytes(
        &loaded->rules, sizeof(flowie_security_rule_t), _Alignof(flowie_security_rule_t),
        provider->max_rules));
  }
  rules_load.provider = provider;
  rules_load.rules = loaded ? &loaded->rules : NULL;
  if (rc == TURBO_OK)
    rc = flowie_security_orm_query_visit(
        connection, transaction, rules_sql, provider->namespace_name, rule_columns,
        sizeof(rule_columns) / sizeof(rule_columns[0]), provider->max_rules,
        flowie_security_rule_visit, &rules_load, &rule_rows);
  if (rc == TURBO_OK && rule_rows == 0u) rc = TURBO_EPROTO;
  if (rc == TURBO_OK)
    rc = flowie_security_orm_status(orm_transaction_commit(transaction, &error));
  else if (transaction)
    (void)orm_transaction_rollback(transaction, &error);
  if (rc == TURBO_OK) {
    bundle_out->policy_version = metadata.policy_version;
    bundle_out->expires_at = metadata.expires_at;
    bundle_out->rules = (const flowie_security_rule_t *)vec_data_const(&loaded->rules);
    bundle_out->rule_count = rule_rows;
    bundle_out->provider_bundle = loaded;
    loaded = NULL;
  }
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
      config->busy_timeout_ms < 0 || config->max_rules == 0u || config->max_rules == SIZE_MAX ||
      !out)
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
