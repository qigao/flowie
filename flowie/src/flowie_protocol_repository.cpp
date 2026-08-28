#include "flowie_protocol_repository.h"
#include "flowie_orm_flow_internal.h"

#include "orm.h"
#include "turbo_error.h"

#include <turbostl/vec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

struct flowie_protocol_repository_s {
  orm_connection_t *connection;
  flowie_protocol_repository_limits_t limits;
  char prefix[64];
};

static orm_string_view_t repo_view(const char *value) { return orm_view(value); }

static orm_value_t repo_u64(uint64_t value) {
  orm_value_t out{};
  out.kind = ORM_VALUE_UINT64;
  out.data.uint64_value = value;
  return out;
}

static orm_value_t repo_i64(int64_t value) {
  orm_value_t out{};
  out.kind = ORM_VALUE_INT64;
  out.data.int64_value = value;
  return out;
}

static orm_value_t repo_text(const char *value) {
  orm_value_t out{};
  out.kind = ORM_VALUE_TEXT;
  out.data.text_value = repo_view(value ? value : "");
  return out;
}

static orm_value_t repo_span(flowie_mqtt_span_t value, bool blob) {
  orm_value_t out{};
  out.kind = blob ? ORM_VALUE_BLOB : ORM_VALUE_TEXT;
  if (blob) out.data.blob_value = {value.data, value.size};
  else out.data.text_value = {(const char *)value.data, value.size};
  return out;
}

static int repo_status(orm_status_t status) { return flowie_orm_status_to_turbo(status); }

static int repo_table(const flowie_protocol_repository_t *repository, const char *name, char *out,
                      size_t capacity) {
  int written;
  if (!repository || !name || !out || capacity == 0u) return TURBO_EINVAL;
  written = std::snprintf(out, capacity, "%s%s", repository->prefix, name);
  return written > 0 && (size_t)written < capacity ? TURBO_OK : TURBO_ERANGE;
}

static int repo_execute_raw(flowie_protocol_repository_t *repository, const char *sql) {
  orm_error_t error;
  orm_query_t *query = nullptr;
  orm_error_init(&error);
  orm_status_t status = orm_raw(repository->connection, repo_view(sql), &query, &error);
  int rc = repo_status(status);
  if (rc == TURBO_OK) rc = flowie_orm_command_execute(query, nullptr, nullptr);
  orm_query_destroy(query);
  return rc;
}

struct repo_schema_version_context {
  int64_t version;
};

static int repo_schema_version_visit(void *ctx, const flowie_orm_row_t *row, size_t row_index) {
  auto *version = static_cast<repo_schema_version_context *>(ctx);
  if (!version || row_index != 0u) return TURBO_EPROTO;
  version->version = flowie_orm_row_int64(row, 0u);
  return TURBO_OK;
}

static int repo_create_schema(flowie_protocol_repository_t *repository) {
  const char *blob = "bytea";
  char sql[2048];
  char name[128];
  int rc;

#define REPO_SCHEMA(table, body)                                                                   \
  do {                                                                                             \
    rc = repo_table(repository, table, name, sizeof(name));                                        \
    if (rc != TURBO_OK) return rc;                                                                 \
    auto format_schema = &std::snprintf;                                                           \
    int written = format_schema(sql, sizeof(sql), "create table if not exists %s(" body ")", name, \
                                blob, blob, blob, blob);                                           \
    if (written <= 0 || (size_t)written >= sizeof(sql)) return TURBO_ERANGE;                       \
    rc = repo_execute_raw(repository, sql);                                                        \
    if (rc != TURBO_OK) return rc;                                                                 \
  } while (0)

  REPO_SCHEMA("meta", "schema_version integer not null");
  REPO_SCHEMA(
      "sessions",
      "client_id text primary key, revision bigint not null, session_id bigint not null, "
      "session_generation bigint not null, mqtt_version integer not null, keep_alive "
      "integer not null, expiry_interval bigint not null, next_packet_id integer not null, "
      "expires_at bigint not null, will_at bigint not null, has_principal integer not null, "
      "principal_id text not null, principal_type text not null, domain_id text not null, "
      "auth_method text not null, principal_scope integer not null, principal_expires_at "
      "bigint not null, policy_version bigint not null");
  REPO_SCHEMA("principal_roles",
              "client_id text not null, position integer not null, role text not null, primary key "
              "(client_id, position)");
  REPO_SCHEMA("principal_groups",
              "client_id text not null, position integer not null, group_id text not null, primary "
              "key (client_id, position)");
  REPO_SCHEMA(
      "subscriptions",
      "client_id text not null, filter text not null, qos integer not null, no_local integer "
      "not null, retain_as_published integer not null, retain_handling integer not null, "
      "subscription_identifier bigint not null, primary key (client_id, filter)");
  REPO_SCHEMA("inflight",
              "client_id text not null, packet_id integer not null, qos integer not null, primary "
              "key (client_id, packet_id)");
  REPO_SCHEMA("deliveries",
              "client_id text not null, packet_id integer not null, qos integer not null, state "
              "integer not null, expires_at bigint not null, packet %s not null, primary key "
              "(client_id, packet_id)");
  REPO_SCHEMA(
      "wills",
      "client_id text primary key, pending integer not null, qos integer not null, retain "
      "integer not null, delay_interval bigint not null, topic text not null, properties %s "
      "not null, payload %s not null");
  REPO_SCHEMA(
      "retained",
      "topic text primary key, revision bigint not null, publisher_session_id bigint not null, "
      "expires_at bigint not null, mqtt_version integer not null, qos integer not null, "
      "properties %s not null, payload %s "
      "not null");
#undef REPO_SCHEMA

  if (repo_table(repository, "meta", name, sizeof(name)) != TURBO_OK) return TURBO_ERANGE;
  std::snprintf(sql, sizeof(sql),
                "insert into %s(schema_version) select %u where not exists "
                "(select 1 from %s)",
                name, FLOWIE_PROTOCOL_REPOSITORY_SCHEMA_VERSION, name);
  return repo_execute_raw(repository, sql);
}

static int repo_validate_schema(flowie_protocol_repository_t *repository) {
  char table[128];
  char sql[256];
  orm_error_t error;
  orm_query_t *query = nullptr;
  repo_schema_version_context version{};
  size_t rows = 0u;
  static const flowie_orm_column_t columns[] = {
      {"schema_version", FLOWIE_ORM_COLUMN_INT64},
  };
  int rc = repo_table(repository, "meta", table, sizeof(table));
  if (rc != TURBO_OK) return rc;
  const int written = std::snprintf(sql, sizeof(sql), "select schema_version from %s", table);
  if (written <= 0 || (size_t)written >= sizeof(sql)) return TURBO_ERANGE;
  orm_error_init(&error);
  orm_status_t status = orm_raw(repository->connection, repo_view(sql), &query, &error);
  rc = repo_status(status);
  if (rc == TURBO_OK)
    rc = flowie_orm_query_visit(query, nullptr, columns, 1u, 2u, 0u, repo_schema_version_visit,
                                &version, &rows);
  orm_query_destroy(query);
  if (rc != TURBO_OK) return rc;
  return rows == 1u && version.version == FLOWIE_PROTOCOL_REPOSITORY_SCHEMA_VERSION ? TURBO_OK
                                                                                    : TURBO_EPROTO;
}

static bool repo_limits_valid(const flowie_protocol_repository_limits_t *limits) {
  return limits && limits->max_sessions != 0u && limits->max_subscriptions_per_session != 0u &&
         limits->max_inflight_per_session != 0u && limits->max_retained_messages != 0u &&
         limits->max_sessions != SIZE_MAX && limits->max_subscriptions_per_session != SIZE_MAX &&
         limits->max_inflight_per_session != SIZE_MAX &&
         limits->max_retained_messages != SIZE_MAX && limits->max_client_id_size != 0u &&
         limits->max_topic_size != 0u && limits->max_packet_size != 0u;
}

static size_t repo_max_result_rows(const flowie_protocol_repository_limits_t *limits) {
  size_t maximum = limits->max_sessions;
  if (maximum < limits->max_subscriptions_per_session)
    maximum = limits->max_subscriptions_per_session;
  if (maximum < limits->max_inflight_per_session) maximum = limits->max_inflight_per_session;
  if (maximum < limits->max_retained_messages) maximum = limits->max_retained_messages;
  if (maximum < FLOWIE_SECURITY_MAX_ROLES) maximum = FLOWIE_SECURITY_MAX_ROLES;
  if (maximum < FLOWIE_SECURITY_MAX_GROUPS) maximum = FLOWIE_SECURITY_MAX_GROUPS;
  return maximum + 1u;
}

static bool repo_cstr_valid(const char *value, size_t capacity, bool required) {
  if (!value || capacity == 0u) return false;
  const void *end = std::memchr(value, '\0', capacity);
  return end && (!required || end != value);
}

int flowie_protocol_repository_open(const flowie_protocol_repository_config_t *config,
                                    flowie_protocol_repository_t **out) {
  flowie_protocol_repository_t *repository = nullptr;
  orm_config_t orm_settings;
  orm_error_t error;
  orm_status_t status;
  int rc = TURBO_EINVAL;
  if (out) *out = nullptr;
  if (!config || config->size < sizeof(*config) || !out || !config->database ||
      config->database->struct_size != sizeof(*config->database) ||
      config->database->abi_version != ORM_C_ABI_VERSION ||
      !repo_limits_valid(&config->limits))
    return TURBO_EINVAL;
  repository = (flowie_protocol_repository_t *)std::calloc(1u, sizeof(*repository));
  if (!repository) return TURBO_ENOMEM;
  repository->limits = config->limits;
  const char *prefix = config->namespace_name && config->namespace_name[0] ? config->namespace_name
                                                                           : "flowie_protocol";
  size_t prefix_size = std::strlen(prefix);
  if (prefix_size > sizeof(repository->prefix) - 2u) goto done;
  for (size_t i = 0u; i < prefix_size; ++i) {
    char c = prefix[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i != 0u && c >= '0' && c <= '9') ||
          c == '_'))
      goto done;
  }
  std::memcpy(repository->prefix, prefix, prefix_size);
  repository->prefix[prefix_size] = '_';
  orm_settings = *config->database;
  orm_settings.max_result_rows = repo_max_result_rows(&config->limits);
  orm_settings.max_parameter_bytes = config->limits.max_packet_size;
  orm_error_init(&error);
  status = orm_connect(&orm_settings, &repository->connection, &error);
  if (status != ORM_STATUS_OK) {
    rc = repo_status(status);
    goto done;
  }
  if (config->create_schema) {
    rc = repo_create_schema(repository);
    if (rc != TURBO_OK) goto done;
  }
  rc = repo_validate_schema(repository);
  if (rc != TURBO_OK) goto done;
  *out = repository;
  repository = nullptr;
  rc = TURBO_OK;
done:
  flowie_protocol_repository_close(repository);
  return rc;
}

void flowie_protocol_repository_close(flowie_protocol_repository_t *repository) {
  if (!repository) return;
  orm_disconnect(repository->connection);
  std::free(repository);
}

static int repo_query_finish(orm_query_t *query, orm_transaction_t *transaction) {
  int rc = flowie_orm_command_execute(query, transaction, nullptr);
  orm_query_destroy(query);
  return rc;
}

static int repo_query_set(orm_query_t *query, const char *column, orm_value_t value) {
  orm_error_t error;
  orm_error_init(&error);
  return repo_status(orm_query_set(query, repo_view(column), value, &error));
}

static int repo_query_where(orm_query_t *query, const char *column, orm_value_t value) {
  orm_error_t error;
  orm_error_init(&error);
  return repo_status(orm_query_where(query, repo_view(column), ORM_COMPARE_EQUAL, value, &error));
}

static int repo_query_new(flowie_protocol_repository_t *repository, const char *table, int kind,
                          orm_query_t **out) {
  char name[128];
  orm_error_t error;
  orm_status_t status;
  if (out) *out = nullptr;
  int rc = repo_table(repository, table, name, sizeof(name));
  if (rc != TURBO_OK || !out) return rc != TURBO_OK ? rc : TURBO_EINVAL;
  orm_error_init(&error);
  status = kind == 1   ? orm_insert(repository->connection, repo_view(name), out, &error)
           : kind == 2 ? orm_update(repository->connection, repo_view(name), out, &error)
           : kind == 3 ? orm_delete(repository->connection, repo_view(name), out, &error)
                       : orm_query_create(repository->connection, repo_view(name), out, &error);
  return repo_status(status);
}

static int repo_delete_by_client(flowie_protocol_repository_t *repository,
                                 orm_transaction_t *transaction, const char *table,
                                 flowie_mqtt_span_t client_id) {
  orm_query_t *query = nullptr;
  int rc = repo_query_new(repository, table, 3, &query);
  if (rc == TURBO_OK) rc = repo_query_where(query, "client_id", repo_span(client_id, false));
  if (rc != TURBO_OK) {
    orm_query_destroy(query);
    return rc;
  }
  return repo_query_finish(query, transaction);
}

struct repo_uint64_context {
  uint64_t value;
};

static int repo_uint64_visit(void *ctx, const flowie_orm_row_t *row, size_t row_index) {
  auto *value = static_cast<repo_uint64_context *>(ctx);
  if (!value || row_index != 0u) return TURBO_EPROTO;
  value->value = flowie_orm_row_uint64(row, 0u);
  return TURBO_OK;
}

static int repo_existing_revision(flowie_protocol_repository_t *repository, const char *table,
                                  const char *key_column, orm_value_t key,
                                  orm_transaction_t *transaction, bool *found, uint64_t *revision) {
  orm_query_t *query = nullptr;
  orm_error_t error;
  repo_uint64_context stored{};
  size_t rows = 0u;
  static const flowie_orm_column_t columns[] = {
      {"revision", FLOWIE_ORM_COLUMN_UINT64},
  };
  int rc;
  *found = false;
  *revision = 0u;
  rc = repo_query_new(repository, table, 0, &query);
  orm_error_init(&error);
  if (rc == TURBO_OK) rc = repo_status(orm_query_add_column(query, repo_view("revision"), &error));
  if (rc == TURBO_OK) rc = repo_query_where(query, key_column, key);
  if (rc == TURBO_OK)
    rc = flowie_orm_query_visit(query, transaction, columns, 1u, 2u, 0u, repo_uint64_visit, &stored,
                                &rows);
  orm_query_destroy(query);
  if (rc == TURBO_OK && rows == 1u) {
    *revision = stored.value;
    *found = true;
  }
  return rc;
}

static int repo_check_cas(bool found, uint64_t stored, uint64_t expected, uint64_t next) {
  if (next == 0u || next <= expected) return TURBO_EINVAL;
  if ((!found && expected != 0u) || (found && stored != expected)) return TURBO_EBUSY;
  return TURBO_OK;
}

static int repo_count_rows(flowie_protocol_repository_t *repository, const char *table,
                           orm_transaction_t *transaction, uint64_t *count) {
  char name[128];
  char sql[256];
  orm_error_t error;
  orm_query_t *query = nullptr;
  repo_uint64_context rows_count{};
  size_t rows = 0u;
  static const flowie_orm_column_t columns[] = {
      {"row_count", FLOWIE_ORM_COLUMN_UINT64},
  };
  int rc = repo_table(repository, table, name, sizeof(name));
  if (rc != TURBO_OK || !count) return rc != TURBO_OK ? rc : TURBO_EINVAL;
  const int written = std::snprintf(sql, sizeof(sql), "select count(*) as row_count from %s", name);
  if (written <= 0 || (size_t)written >= sizeof(sql)) return TURBO_ERANGE;
  orm_error_init(&error);
  orm_status_t status = orm_raw(repository->connection, repo_view(sql), &query, &error);
  rc = repo_status(status);
  if (rc == TURBO_OK)
    rc = flowie_orm_query_visit(query, transaction, columns, 1u, 2u, 0u, repo_uint64_visit,
                                &rows_count, &rows);
  orm_query_destroy(query);
  if (rc == TURBO_OK && rows != 1u) rc = TURBO_EPROTO;
  if (rc == TURBO_OK) *count = rows_count.value;
  return rc;
}

static int repo_insert_session(flowie_protocol_repository_t *repository,
                               orm_transaction_t *transaction,
                               const flowie_protocol_session_row_t *row) {
  orm_query_t *query = nullptr;
  const flowie_security_principal_t *p = &row->principal;
  int rc = repo_query_new(repository, "sessions", 1, &query);
#define REPO_SET(column, value)                                                                    \
  do {                                                                                             \
    if (rc == TURBO_OK) rc = repo_query_set(query, column, value);                                 \
  } while (0)
  REPO_SET("client_id", repo_span(row->client_id, false));
  REPO_SET("revision", repo_u64(row->revision));
  REPO_SET("session_id", repo_u64(row->session_id));
  REPO_SET("session_generation", repo_u64(row->session_generation));
  REPO_SET("mqtt_version", repo_i64(row->mqtt_version));
  REPO_SET("keep_alive", repo_i64(row->keep_alive));
  REPO_SET("expiry_interval", repo_u64(row->session_expiry_interval));
  REPO_SET("next_packet_id", repo_i64(row->next_delivery_packet_id));
  REPO_SET("expires_at", repo_u64(row->expiry_at_epoch_seconds));
  REPO_SET("will_at", repo_u64(row->will_at_epoch_seconds));
  REPO_SET("has_principal", repo_i64(row->has_principal));
  REPO_SET("principal_id", repo_text(row->has_principal ? p->principal_id : ""));
  REPO_SET("principal_type", repo_text(row->has_principal ? p->principal_type : ""));
  REPO_SET("domain_id", repo_text(row->has_principal ? p->domain_id : ""));
  REPO_SET("auth_method", repo_text(row->has_principal ? p->auth_method : ""));
  REPO_SET("principal_scope", repo_i64(row->has_principal ? p->scope : 0));
  REPO_SET("principal_expires_at", repo_u64(row->has_principal ? p->expires_at : 0u));
  REPO_SET("policy_version", repo_u64(row->has_principal ? p->policy_version : 0u));
#undef REPO_SET
  if (rc != TURBO_OK) {
    orm_query_destroy(query);
    return rc;
  }
  return repo_query_finish(query, transaction);
}

static int repo_insert_child(flowie_protocol_repository_t *repository,
                             orm_transaction_t *transaction, const char *table,
                             flowie_mqtt_span_t client_id, const char *const *columns,
                             const orm_value_t *values, size_t count) {
  orm_query_t *query = nullptr;
  int rc = repo_query_new(repository, table, 1, &query);
  if (rc == TURBO_OK) rc = repo_query_set(query, "client_id", repo_span(client_id, false));
  for (size_t i = 0u; rc == TURBO_OK && i < count; ++i)
    rc = repo_query_set(query, columns[i], values[i]);
  if (rc != TURBO_OK) {
    orm_query_destroy(query);
    return rc;
  }
  return repo_query_finish(query, transaction);
}

static int repo_replace_session_children(flowie_protocol_repository_t *repository,
                                         orm_transaction_t *transaction,
                                         const flowie_protocol_session_row_t *row) {
  static const char *const child_tables[] = {"principal_roles", "principal_groups", "subscriptions",
                                             "inflight",        "deliveries",       "wills"};
  int rc = TURBO_OK;
  for (const char *table : child_tables) {
    rc = repo_delete_by_client(repository, transaction, table, row->client_id);
    if (rc != TURBO_OK) return rc;
  }
  if (row->has_principal) {
    const char *columns[] = {"position", "role"};
    for (uint32_t i = 0u; i < row->principal.role_count; ++i) {
      orm_value_t values[] = {repo_i64(i), repo_text(row->principal.roles[i])};
      rc = repo_insert_child(repository, transaction, "principal_roles", row->client_id, columns,
                             values, 2u);
      if (rc != TURBO_OK) return rc;
    }
    columns[1] = "group_id";
    for (uint32_t i = 0u; i < row->principal.group_count; ++i) {
      orm_value_t values[] = {repo_i64(i), repo_text(row->principal.groups[i])};
      rc = repo_insert_child(repository, transaction, "principal_groups", row->client_id, columns,
                             values, 2u);
      if (rc != TURBO_OK) return rc;
    }
  }
  for (size_t i = 0u; i < row->subscription_count; ++i) {
    const auto &entry = row->subscriptions[i];
    const char *columns[] = {"filter",          "qos",
                             "no_local",        "retain_as_published",
                             "retain_handling", "subscription_identifier"};
    orm_value_t values[] = {
        repo_span(entry.filter, false),  repo_i64(entry.qos),
        repo_i64(entry.no_local),        repo_i64(entry.retain_as_published),
        repo_i64(entry.retain_handling), repo_u64(entry.subscription_identifier)};
    rc = repo_insert_child(repository, transaction, "subscriptions", row->client_id, columns,
                           values, 6u);
    if (rc != TURBO_OK) return rc;
  }
  for (size_t i = 0u; i < row->inflight_count; ++i) {
    const char *columns[] = {"packet_id", "qos"};
    orm_value_t values[] = {repo_i64(row->inflight[i].packet_id), repo_i64(row->inflight[i].qos)};
    rc =
        repo_insert_child(repository, transaction, "inflight", row->client_id, columns, values, 2u);
    if (rc != TURBO_OK) return rc;
  }
  for (size_t i = 0u; i < row->delivery_count; ++i) {
    const auto &entry = row->deliveries[i];
    const char *columns[] = {"packet_id", "qos", "state", "expires_at", "packet"};
    orm_value_t values[] = {repo_i64(entry.packet_id), repo_i64(entry.qos), repo_i64(entry.state),
                            repo_u64(entry.expiry_at_epoch_seconds), repo_span(entry.packet, true)};
    rc = repo_insert_child(repository, transaction, "deliveries", row->client_id, columns, values,
                           5u);
    if (rc != TURBO_OK) return rc;
  }
  if (row->will.present) {
    const char *columns[] = {"pending", "qos",        "retain", "delay_interval",
                             "topic",   "properties", "payload"};
    orm_value_t values[] = {
        repo_i64(row->will.pending),       repo_i64(row->will.qos),
        repo_i64(row->will.retain),        repo_u64(row->will.delay_interval),
        repo_span(row->will.topic, false), repo_span(row->will.properties, true),
        repo_span(row->will.payload, true)};
    rc = repo_insert_child(repository, transaction, "wills", row->client_id, columns, values, 7u);
  }
  return rc;
}

int flowie_protocol_repository_session_save(flowie_protocol_repository_t *repository,
                                            const flowie_protocol_session_row_t *row) {
  orm_transaction_t *transaction = nullptr;
  orm_error_t error;
  bool found = false;
  uint64_t stored = 0u;
  uint64_t session_count = 0u;
  int rc;
  if (!repository || !row || row->size < sizeof(*row) || !row->client_id.data ||
      row->client_id.size == 0u || row->client_id.size > repository->limits.max_client_id_size ||
      row->subscription_count > repository->limits.max_subscriptions_per_session ||
      row->inflight_count + row->delivery_count > repository->limits.max_inflight_per_session ||
      (!row->subscriptions && row->subscription_count) || (!row->inflight && row->inflight_count) ||
      (!row->deliveries && row->delivery_count))
    return TURBO_EINVAL;
  if (row->session_id == 0u || row->session_generation == 0u ||
      !flowie_mqtt_version_is_supported(row->mqtt_version) || row->has_principal < 0 ||
      row->has_principal > 1 ||
      (row->has_principal &&
       (row->principal.role_count > FLOWIE_SECURITY_MAX_ROLES ||
        row->principal.group_count > FLOWIE_SECURITY_MAX_GROUPS ||
        row->principal.scope < FLOWIE_SECURITY_SCOPE_SELF ||
        row->principal.scope > FLOWIE_SECURITY_SCOPE_SYSTEM ||
        !repo_cstr_valid(row->principal.principal_id, sizeof(row->principal.principal_id), true) ||
        !repo_cstr_valid(row->principal.principal_type, sizeof(row->principal.principal_type),
                         true) ||
        !repo_cstr_valid(row->principal.domain_id, sizeof(row->principal.domain_id), false) ||
        !repo_cstr_valid(row->principal.auth_method, sizeof(row->principal.auth_method), true))))
    return TURBO_EINVAL;
  if (row->has_principal) {
    for (uint32_t i = 0u; i < row->principal.role_count; ++i)
      if (!repo_cstr_valid(row->principal.roles[i], sizeof(row->principal.roles[i]), true))
        return TURBO_EINVAL;
    for (uint32_t i = 0u; i < row->principal.group_count; ++i)
      if (!repo_cstr_valid(row->principal.groups[i], sizeof(row->principal.groups[i]), true))
        return TURBO_EINVAL;
  }
  for (size_t i = 0u; i < row->subscription_count; ++i)
    if (!row->subscriptions[i].filter.data || row->subscriptions[i].filter.size == 0u ||
        row->subscriptions[i].filter.size > repository->limits.max_topic_size ||
        row->subscriptions[i].qos > 2u || row->subscriptions[i].no_local > 1u ||
        row->subscriptions[i].retain_as_published > 1u ||
        row->subscriptions[i].retain_handling > 2u)
      return TURBO_EINVAL;
  for (size_t i = 0u; i < row->inflight_count; ++i)
    if (row->inflight[i].packet_id == 0u || row->inflight[i].qos > 2u) return TURBO_EINVAL;
  for (size_t i = 0u; i < row->delivery_count; ++i)
    if (row->deliveries[i].packet_id == 0u || row->deliveries[i].qos > 2u ||
        row->deliveries[i].state == 0u || !row->deliveries[i].packet.data ||
        row->deliveries[i].packet.size == 0u ||
        row->deliveries[i].packet.size > repository->limits.max_packet_size)
      return TURBO_EINVAL;
  if (row->will.present &&
      (row->will.pending < 0 || row->will.pending > 1 || row->will.qos > 2u ||
       row->will.retain > 1u || !row->will.topic.data || row->will.topic.size == 0u ||
       row->will.topic.size > repository->limits.max_topic_size ||
       row->will.properties.size > repository->limits.max_packet_size ||
       row->will.payload.size > repository->limits.max_packet_size))
    return TURBO_EINVAL;
  orm_error_init(&error);
  rc = repo_status(orm_transaction_begin(repository->connection, ORM_ISOLATION_SERIALIZABLE,
                                         &transaction, &error));
  if (rc == TURBO_OK)
    rc = repo_existing_revision(repository, "sessions", "client_id",
                                repo_span(row->client_id, false), transaction, &found, &stored);
  if (rc == TURBO_OK && !found)
    rc = repo_count_rows(repository, "sessions", transaction, &session_count);
  if (rc == TURBO_OK && !found && session_count >= repository->limits.max_sessions)
    rc = TURBO_ENOSPC;
  if (rc == TURBO_OK) rc = repo_check_cas(found, stored, row->expected_revision, row->revision);
  if (rc == TURBO_OK && found)
    rc = repo_delete_by_client(repository, transaction, "sessions", row->client_id);
  if (rc == TURBO_OK) rc = repo_insert_session(repository, transaction, row);
  if (rc == TURBO_OK) rc = repo_replace_session_children(repository, transaction, row);
  if (rc == TURBO_OK) rc = repo_status(orm_transaction_commit(transaction, &error));
  else (void)orm_transaction_rollback(transaction, &error);
  orm_transaction_destroy(transaction);
  return rc;
}

int flowie_protocol_repository_session_delete(flowie_protocol_repository_t *repository,
                                              flowie_mqtt_span_t client_id,
                                              uint64_t expected_revision) {
  static const char *const tables[] = {"principal_roles", "principal_groups", "subscriptions",
                                       "inflight",        "deliveries",       "wills",
                                       "sessions"};
  orm_transaction_t *transaction = nullptr;
  orm_error_t error;
  bool found = false;
  uint64_t stored = 0u;
  int rc;
  if (!repository || !client_id.data || client_id.size == 0u || expected_revision == 0u)
    return TURBO_EINVAL;
  orm_error_init(&error);
  rc = repo_status(orm_transaction_begin(repository->connection, ORM_ISOLATION_SERIALIZABLE,
                                         &transaction, &error));
  if (rc == TURBO_OK)
    rc = repo_existing_revision(repository, "sessions", "client_id", repo_span(client_id, false),
                                transaction, &found, &stored);
  if (rc == TURBO_OK && (!found || stored != expected_revision)) rc = TURBO_EBUSY;
  for (const char *table : tables) {
    if (rc == TURBO_OK) rc = repo_delete_by_client(repository, transaction, table, client_id);
  }
  if (rc == TURBO_OK) rc = repo_status(orm_transaction_commit(transaction, &error));
  else (void)orm_transaction_rollback(transaction, &error);
  orm_transaction_destroy(transaction);
  return rc;
}

static int repo_retained_valid(const flowie_protocol_repository_t *repository,
                               const flowie_protocol_retained_row_t *row) {
  return repository && row && row->size >= sizeof(*row) && row->topic.data &&
         row->topic.size != 0u && row->topic.size <= repository->limits.max_topic_size &&
         row->revision != 0u && row->revision > row->expected_revision && row->qos <= 2u &&
         flowie_mqtt_version_is_supported(row->mqtt_version) &&
         row->properties.size <= repository->limits.max_packet_size &&
         row->payload.size <= repository->limits.max_packet_size &&
         (row->properties.data || row->properties.size == 0u) &&
         (row->payload.data || row->payload.size == 0u);
}

int flowie_protocol_repository_retained_save(flowie_protocol_repository_t *repository,
                                             const flowie_protocol_retained_row_t *row) {
  orm_transaction_t *transaction = nullptr;
  orm_query_t *query = nullptr;
  orm_error_t error;
  bool found = false;
  uint64_t stored = 0u;
  uint64_t retained_count = 0u;
  int rc;
  if (!repo_retained_valid(repository, row)) return TURBO_EINVAL;
  orm_error_init(&error);
  rc = repo_status(orm_transaction_begin(repository->connection, ORM_ISOLATION_SERIALIZABLE,
                                         &transaction, &error));
  if (rc == TURBO_OK)
    rc = repo_existing_revision(repository, "retained", "topic", repo_span(row->topic, false),
                                transaction, &found, &stored);
  if (rc == TURBO_OK && !found)
    rc = repo_count_rows(repository, "retained", transaction, &retained_count);
  if (rc == TURBO_OK && !found && retained_count >= repository->limits.max_retained_messages)
    rc = TURBO_ENOSPC;
  if (rc == TURBO_OK) rc = repo_check_cas(found, stored, row->expected_revision, row->revision);
  if (rc == TURBO_OK && found) {
    rc = repo_query_new(repository, "retained", 3, &query);
    if (rc == TURBO_OK) rc = repo_query_where(query, "topic", repo_span(row->topic, false));
    if (rc == TURBO_OK) rc = repo_query_finish(query, transaction);
    else orm_query_destroy(query);
    query = nullptr;
  }
  if (rc == TURBO_OK) rc = repo_query_new(repository, "retained", 1, &query);
#define RETAINED_SET(column, value)                                                                \
  do {                                                                                             \
    if (rc == TURBO_OK) rc = repo_query_set(query, column, value);                                 \
  } while (0)
  RETAINED_SET("topic", repo_span(row->topic, false));
  RETAINED_SET("revision", repo_u64(row->revision));
  RETAINED_SET("publisher_session_id", repo_u64(row->publisher_session_id));
  RETAINED_SET("expires_at", repo_u64(row->expiry_at_epoch_seconds));
  RETAINED_SET("mqtt_version", repo_i64(row->mqtt_version));
  RETAINED_SET("qos", repo_i64(row->qos));
  RETAINED_SET("properties", repo_span(row->properties, true));
  RETAINED_SET("payload", repo_span(row->payload, true));
#undef RETAINED_SET
  if (rc == TURBO_OK) rc = repo_query_finish(query, transaction);
  else orm_query_destroy(query);
  if (rc == TURBO_OK) rc = repo_status(orm_transaction_commit(transaction, &error));
  else (void)orm_transaction_rollback(transaction, &error);
  orm_transaction_destroy(transaction);
  return rc;
}

int flowie_protocol_repository_retained_delete(flowie_protocol_repository_t *repository,
                                               flowie_mqtt_span_t topic,
                                               uint64_t expected_revision) {
  orm_transaction_t *transaction = nullptr;
  orm_query_t *query = nullptr;
  orm_error_t error;
  bool found = false;
  uint64_t stored = 0u;
  int rc;
  if (!repository || !topic.data || topic.size == 0u || expected_revision == 0u)
    return TURBO_EINVAL;
  orm_error_init(&error);
  rc = repo_status(orm_transaction_begin(repository->connection, ORM_ISOLATION_SERIALIZABLE,
                                         &transaction, &error));
  if (rc == TURBO_OK)
    rc = repo_existing_revision(repository, "retained", "topic", repo_span(topic, false),
                                transaction, &found, &stored);
  if (rc == TURBO_OK && (!found || stored != expected_revision)) rc = TURBO_EBUSY;
  if (rc == TURBO_OK) rc = repo_query_new(repository, "retained", 3, &query);
  if (rc == TURBO_OK) rc = repo_query_where(query, "topic", repo_span(topic, false));
  if (rc == TURBO_OK) rc = repo_query_finish(query, transaction);
  else orm_query_destroy(query);
  if (rc == TURBO_OK) rc = repo_status(orm_transaction_commit(transaction, &error));
  else (void)orm_transaction_rollback(transaction, &error);
  orm_transaction_destroy(transaction);
  return rc;
}

static size_t repo_max_buffer_bytes(const flowie_protocol_repository_t *repository) {
  size_t maximum = repository->limits.max_packet_size;
  if (repository->limits.max_topic_size > maximum) maximum = repository->limits.max_topic_size;
  if (repository->limits.max_client_id_size > maximum)
    maximum = repository->limits.max_client_id_size;
  return maximum;
}

static int repo_select_visit(flowie_protocol_repository_t *repository, const char *table,
                             const flowie_orm_column_t *columns, size_t column_count,
                             const char *key_column, const orm_value_t *key,
                             const char *order_column, size_t max_rows,
                             flowie_orm_row_visit_fn visit, void *visit_ctx, size_t *row_count) {
  orm_query_t *query = nullptr;
  orm_error_t error;
  int rc = repo_query_new(repository, table, 0, &query);
  orm_error_init(&error);
  for (size_t i = 0u; rc == TURBO_OK && i < column_count; ++i)
    rc = repo_status(orm_query_add_column(query, repo_view(columns[i].name), &error));
  if (rc == TURBO_OK && key_column && key) rc = repo_query_where(query, key_column, *key);
  if (rc == TURBO_OK && order_column)
    rc = repo_status(
        orm_query_order_by(query, repo_view(order_column), ORM_ORDER_ASCENDING, &error));
  if (rc == TURBO_OK)
    rc = flowie_orm_query_visit(query, nullptr, columns, column_count, max_rows,
                                repo_max_buffer_bytes(repository), visit, visit_ctx, row_count);
  orm_query_destroy(query);
  return rc;
}

static flowie_mqtt_span_t repo_buffer_span(tstr value) {
  return {(const uint8_t *)value, tstr_len(value)};
}

struct repo_retained_visit_context {
  flowie_protocol_repository_t *repository;
  flowie_protocol_retained_visit_fn visit;
  void *visit_ctx;
};

static int repo_retained_visit_row(void *ctx, const flowie_orm_row_t *source, size_t row_index) {
  auto *state = static_cast<repo_retained_visit_context *>(ctx);
  flowie_protocol_retained_row_t row = FLOWIE_PROTOCOL_RETAINED_ROW_INIT;
  const int64_t mqtt_version = flowie_orm_row_int64(source, 4u);
  const int64_t qos = flowie_orm_row_int64(source, 5u);
  (void)row_index;
  if (!state || !state->repository || !state->visit) return TURBO_EINVAL;
  row.topic = repo_buffer_span(flowie_orm_row_buffer(source, 0u));
  row.revision = flowie_orm_row_uint64(source, 1u);
  row.publisher_session_id = flowie_orm_row_uint64(source, 2u);
  row.expiry_at_epoch_seconds = flowie_orm_row_uint64(source, 3u);
  row.properties = repo_buffer_span(flowie_orm_row_buffer(source, 6u));
  row.payload = repo_buffer_span(flowie_orm_row_buffer(source, 7u));
  row.expected_revision = row.revision;
  if (qos < 0 || qos > 2 ||
      !flowie_mqtt_version_is_supported((flowie_mqtt_version_t)mqtt_version) || !row.topic.data ||
      row.topic.size == 0u || row.topic.size > state->repository->limits.max_topic_size ||
      row.revision == 0u || row.properties.size > state->repository->limits.max_packet_size ||
      row.payload.size > state->repository->limits.max_packet_size)
    return TURBO_EPROTO;
  row.mqtt_version = (flowie_mqtt_version_t)mqtt_version;
  row.qos = (uint8_t)qos;
  return state->visit(state->visit_ctx, &row);
}

int flowie_protocol_repository_retained_visit(flowie_protocol_repository_t *repository,
                                              flowie_protocol_retained_visit_fn visit,
                                              void *visit_ctx) {
  static const flowie_orm_column_t columns[] = {
      {"topic", FLOWIE_ORM_COLUMN_TEXT},
      {"revision", FLOWIE_ORM_COLUMN_UINT64},
      {"publisher_session_id", FLOWIE_ORM_COLUMN_UINT64},
      {"expires_at", FLOWIE_ORM_COLUMN_UINT64},
      {"mqtt_version", FLOWIE_ORM_COLUMN_INT64},
      {"qos", FLOWIE_ORM_COLUMN_INT64},
      {"properties", FLOWIE_ORM_COLUMN_BLOB},
      {"payload", FLOWIE_ORM_COLUMN_BLOB},
  };
  repo_retained_visit_context ctx{repository, visit, visit_ctx};
  if (!repository || !visit) return TURBO_EINVAL;
  return repo_select_visit(repository, "retained", columns, sizeof(columns) / sizeof(columns[0]),
                           nullptr, nullptr, nullptr, repository->limits.max_retained_messages,
                           repo_retained_visit_row, &ctx, nullptr);
}

static int repo_copy_text(tstr source, char *out, size_t capacity, bool required) {
  const flowie_mqtt_span_t value = repo_buffer_span(source);
  if (value.size >= capacity || (required && value.size == 0u) ||
      (value.size != 0u && std::memchr(value.data, '\0', value.size)))
    return TURBO_EPROTO;
  if (value.size != 0u) std::memcpy(out, value.data, value.size);
  out[value.size] = '\0';
  return TURBO_OK;
}

struct repo_session_record {
  tstr client_id;
  uint64_t revision;
  uint64_t session_id;
  uint64_t session_generation;
  int64_t mqtt_version;
  int64_t keep_alive;
  uint64_t expiry_interval;
  int64_t next_packet_id;
  uint64_t expires_at;
  uint64_t will_at;
  int64_t has_principal;
  tstr principal_id;
  tstr principal_type;
  tstr domain_id;
  tstr auth_method;
  int64_t principal_scope;
  uint64_t principal_expires_at;
  uint64_t policy_version;
};

static void repo_session_record_destroy(repo_session_record *record) {
  if (!record) return;
  tstr_freep(&record->client_id);
  tstr_freep(&record->principal_id);
  tstr_freep(&record->principal_type);
  tstr_freep(&record->domain_id);
  tstr_freep(&record->auth_method);
}

static int repo_dup_buffer(tstr source, tstr *out) {
  if (!out) return TURBO_EINVAL;
  *out = tstr_dup_len(source ? source : "", tstr_len(source));
  return *out ? TURBO_OK : TURBO_ENOMEM;
}

struct repo_session_collect_context {
  vec_t *records;
};

static int repo_vec_status(stl_status status) {
  switch (status) {
  case STL_OK:
    return TURBO_OK;
  case STL_OUT_OF_MEMORY:
    return TURBO_ENOMEM;
  case STL_CAPACITY_EXCEEDED:
    return TURBO_ENOSPC;
  case STL_INVALID_ARGUMENT:
    return TURBO_EINVAL;
  default:
    return TURBO_EIO;
  }
}

static int repo_session_collect(void *ctx, const flowie_orm_row_t *row, size_t row_index) {
  auto *state = static_cast<repo_session_collect_context *>(ctx);
  repo_session_record record{};
  int rc;
  if (!state || !state->records || row_index != vec_size(state->records)) return TURBO_EINVAL;
  rc = repo_dup_buffer(flowie_orm_row_buffer(row, 0u), &record.client_id);
  record.revision = flowie_orm_row_uint64(row, 1u);
  record.session_id = flowie_orm_row_uint64(row, 2u);
  record.session_generation = flowie_orm_row_uint64(row, 3u);
  record.mqtt_version = flowie_orm_row_int64(row, 4u);
  record.keep_alive = flowie_orm_row_int64(row, 5u);
  record.expiry_interval = flowie_orm_row_uint64(row, 6u);
  record.next_packet_id = flowie_orm_row_int64(row, 7u);
  record.expires_at = flowie_orm_row_uint64(row, 8u);
  record.will_at = flowie_orm_row_uint64(row, 9u);
  record.has_principal = flowie_orm_row_int64(row, 10u);
  if (rc == TURBO_OK) rc = repo_dup_buffer(flowie_orm_row_buffer(row, 11u), &record.principal_id);
  if (rc == TURBO_OK) rc = repo_dup_buffer(flowie_orm_row_buffer(row, 12u), &record.principal_type);
  if (rc == TURBO_OK) rc = repo_dup_buffer(flowie_orm_row_buffer(row, 13u), &record.domain_id);
  if (rc == TURBO_OK) rc = repo_dup_buffer(flowie_orm_row_buffer(row, 14u), &record.auth_method);
  record.principal_scope = flowie_orm_row_int64(row, 15u);
  record.principal_expires_at = flowie_orm_row_uint64(row, 16u);
  record.policy_version = flowie_orm_row_uint64(row, 17u);
  if (rc == TURBO_OK) rc = repo_vec_status(vec_push(state->records, &record));
  /* The byte vector becomes the sole owner of the copied tstr handles. */
  if (rc == TURBO_OK) std::memset(&record, 0, sizeof(record));
  else repo_session_record_destroy(&record);
  return rc;
}

struct repo_principal_child_context {
  flowie_security_principal_t *principal;
  bool roles;
};

static int repo_principal_child_visit(void *ctx, const flowie_orm_row_t *row, size_t row_index) {
  auto *state = static_cast<repo_principal_child_context *>(ctx);
  const int64_t position = flowie_orm_row_int64(row, 0u);
  if (!state || !state->principal || position != (int64_t)row_index) return TURBO_EPROTO;
  return state->roles
             ? repo_copy_text(flowie_orm_row_buffer(row, 1u), state->principal->roles[row_index],
                              sizeof(state->principal->roles[row_index]), true)
             : repo_copy_text(flowie_orm_row_buffer(row, 1u), state->principal->groups[row_index],
                              sizeof(state->principal->groups[row_index]), true);
}

struct repo_session_children {
  flowie_protocol_session_row_t *row;
  flowie_protocol_repository_t *repository;
  tstr *subscription_filters;
  tstr *delivery_packets;
  tstr will_topic;
  tstr will_properties;
  tstr will_payload;
};

static int repo_subscription_visit(void *ctx, const flowie_orm_row_t *source, size_t row_index) {
  auto *state = static_cast<repo_session_children *>(ctx);
  auto *row =
      &const_cast<flowie_protocol_subscription_row_t *>(state->row->subscriptions)[row_index];
  const int64_t qos = flowie_orm_row_int64(source, 1u);
  const int64_t no_local = flowie_orm_row_int64(source, 2u);
  const int64_t rap = flowie_orm_row_int64(source, 3u);
  const int64_t handling = flowie_orm_row_int64(source, 4u);
  const uint64_t identifier = flowie_orm_row_uint64(source, 5u);
  tstr filter = flowie_orm_row_buffer(source, 0u);
  int rc;
  if (!filter || tstr_len(filter) == 0u ||
      tstr_len(filter) > state->repository->limits.max_topic_size || qos < 0 || qos > 2 ||
      no_local < 0 || no_local > 1 || rap < 0 || rap > 1 || handling < 0 || handling > 2 ||
      identifier > UINT32_MAX)
    return TURBO_EPROTO;
  rc = repo_dup_buffer(filter, &state->subscription_filters[row_index]);
  if (rc != TURBO_OK) return rc;
  row->filter = repo_buffer_span(state->subscription_filters[row_index]);
  row->qos = (uint8_t)qos;
  row->no_local = (uint8_t)no_local;
  row->retain_as_published = (uint8_t)rap;
  row->retain_handling = (uint8_t)handling;
  row->subscription_identifier = (uint32_t)identifier;
  return TURBO_OK;
}

static int repo_inflight_visit(void *ctx, const flowie_orm_row_t *source, size_t row_index) {
  auto *state = static_cast<repo_session_children *>(ctx);
  auto *row = &const_cast<flowie_protocol_inflight_row_t *>(state->row->inflight)[row_index];
  const int64_t packet_id = flowie_orm_row_int64(source, 0u);
  const int64_t qos = flowie_orm_row_int64(source, 1u);
  if (packet_id <= 0 || packet_id > UINT16_MAX || qos < 0 || qos > 2) return TURBO_EPROTO;
  row->packet_id = (uint16_t)packet_id;
  row->qos = (uint8_t)qos;
  return TURBO_OK;
}

static int repo_delivery_visit(void *ctx, const flowie_orm_row_t *source, size_t row_index) {
  auto *state = static_cast<repo_session_children *>(ctx);
  auto *row = &const_cast<flowie_protocol_delivery_row_t *>(state->row->deliveries)[row_index];
  const int64_t packet_id = flowie_orm_row_int64(source, 0u);
  const int64_t qos = flowie_orm_row_int64(source, 1u);
  const int64_t delivery_state = flowie_orm_row_int64(source, 2u);
  tstr packet = flowie_orm_row_buffer(source, 4u);
  int rc;
  if (!packet || packet_id <= 0 || packet_id > UINT16_MAX || qos < 0 || qos > 2 ||
      delivery_state <= 0 || delivery_state > UINT8_MAX ||
      tstr_len(packet) > state->repository->limits.max_packet_size)
    return TURBO_EPROTO;
  rc = repo_dup_buffer(packet, &state->delivery_packets[row_index]);
  if (rc != TURBO_OK) return rc;
  row->packet_id = (uint16_t)packet_id;
  row->qos = (uint8_t)qos;
  row->state = (uint8_t)delivery_state;
  row->expiry_at_epoch_seconds = flowie_orm_row_uint64(source, 3u);
  row->packet = repo_buffer_span(state->delivery_packets[row_index]);
  return TURBO_OK;
}

static int repo_will_visit(void *ctx, const flowie_orm_row_t *source, size_t row_index) {
  auto *state = static_cast<repo_session_children *>(ctx);
  const int64_t pending = flowie_orm_row_int64(source, 0u);
  const int64_t qos = flowie_orm_row_int64(source, 1u);
  const int64_t retain = flowie_orm_row_int64(source, 2u);
  const uint64_t delay = flowie_orm_row_uint64(source, 3u);
  int rc;
  if (row_index != 0u || pending < 0 || pending > 1 || qos < 0 || qos > 2 || retain < 0 ||
      retain > 1 || delay > UINT32_MAX)
    return TURBO_EPROTO;
  rc = repo_dup_buffer(flowie_orm_row_buffer(source, 4u), &state->will_topic);
  if (rc == TURBO_OK)
    rc = repo_dup_buffer(flowie_orm_row_buffer(source, 5u), &state->will_properties);
  if (rc == TURBO_OK) rc = repo_dup_buffer(flowie_orm_row_buffer(source, 6u), &state->will_payload);
  if (rc != TURBO_OK) return rc;
  state->row->will.present = 1;
  state->row->will.pending = (int)pending;
  state->row->will.qos = (uint8_t)qos;
  state->row->will.retain = (uint8_t)retain;
  state->row->will.delay_interval = (uint32_t)delay;
  state->row->will.topic = repo_buffer_span(state->will_topic);
  state->row->will.properties = repo_buffer_span(state->will_properties);
  state->row->will.payload = repo_buffer_span(state->will_payload);
  return TURBO_OK;
}

static void repo_session_children_cleanup(repo_session_children *children) {
  size_t index;
  if (!children || !children->row) return;
  for (index = 0u; index < children->row->subscription_count; ++index)
    tstr_freep(&children->subscription_filters[index]);
  for (index = 0u; index < children->row->delivery_count; ++index)
    tstr_freep(&children->delivery_packets[index]);
  tstr_freep(&children->will_topic);
  tstr_freep(&children->will_properties);
  tstr_freep(&children->will_payload);
  std::free(children->subscription_filters);
  std::free(children->delivery_packets);
  std::free((void *)children->row->subscriptions);
  std::free((void *)children->row->inflight);
  std::free((void *)children->row->deliveries);
  children->row->subscriptions = nullptr;
  children->row->inflight = nullptr;
  children->row->deliveries = nullptr;
}

static int repo_session_load_children(flowie_protocol_repository_t *repository,
                                      flowie_protocol_session_row_t *row,
                                      repo_session_children *children) {
  static const flowie_orm_column_t role_columns[] = {{"position", FLOWIE_ORM_COLUMN_INT64},
                                                     {"role", FLOWIE_ORM_COLUMN_TEXT}};
  static const flowie_orm_column_t group_columns[] = {{"position", FLOWIE_ORM_COLUMN_INT64},
                                                      {"group_id", FLOWIE_ORM_COLUMN_TEXT}};
  static const flowie_orm_column_t subscription_columns[] = {
      {"filter", FLOWIE_ORM_COLUMN_TEXT},
      {"qos", FLOWIE_ORM_COLUMN_INT64},
      {"no_local", FLOWIE_ORM_COLUMN_INT64},
      {"retain_as_published", FLOWIE_ORM_COLUMN_INT64},
      {"retain_handling", FLOWIE_ORM_COLUMN_INT64},
      {"subscription_identifier", FLOWIE_ORM_COLUMN_UINT64}};
  static const flowie_orm_column_t inflight_columns[] = {{"packet_id", FLOWIE_ORM_COLUMN_INT64},
                                                         {"qos", FLOWIE_ORM_COLUMN_INT64}};
  static const flowie_orm_column_t delivery_columns[] = {{"packet_id", FLOWIE_ORM_COLUMN_INT64},
                                                         {"qos", FLOWIE_ORM_COLUMN_INT64},
                                                         {"state", FLOWIE_ORM_COLUMN_INT64},
                                                         {"expires_at", FLOWIE_ORM_COLUMN_UINT64},
                                                         {"packet", FLOWIE_ORM_COLUMN_BLOB}};
  static const flowie_orm_column_t will_columns[] = {
      {"pending", FLOWIE_ORM_COLUMN_INT64}, {"qos", FLOWIE_ORM_COLUMN_INT64},
      {"retain", FLOWIE_ORM_COLUMN_INT64},  {"delay_interval", FLOWIE_ORM_COLUMN_UINT64},
      {"topic", FLOWIE_ORM_COLUMN_TEXT},    {"properties", FLOWIE_ORM_COLUMN_BLOB},
      {"payload", FLOWIE_ORM_COLUMN_BLOB}};
  const orm_value_t key = repo_span(row->client_id, false);
  repo_principal_child_context roles{&row->principal, true};
  repo_principal_child_context groups{&row->principal, false};
  size_t role_count = 0u, group_count = 0u, will_count = 0u;
  int rc;
  std::memset(children, 0, sizeof(*children));
  children->row = row;
  children->repository = repository;
  row->subscriptions = (flowie_protocol_subscription_row_t *)std::calloc(
      repository->limits.max_subscriptions_per_session, sizeof(*row->subscriptions));
  children->subscription_filters =
      (tstr *)std::calloc(repository->limits.max_subscriptions_per_session, sizeof(tstr));
  row->inflight = (flowie_protocol_inflight_row_t *)std::calloc(
      repository->limits.max_inflight_per_session, sizeof(*row->inflight));
  row->deliveries = (flowie_protocol_delivery_row_t *)std::calloc(
      repository->limits.max_inflight_per_session, sizeof(*row->deliveries));
  children->delivery_packets =
      (tstr *)std::calloc(repository->limits.max_inflight_per_session, sizeof(tstr));
  if (!row->subscriptions || !children->subscription_filters || !row->inflight ||
      !row->deliveries || !children->delivery_packets)
    return TURBO_ENOMEM;
  rc = repo_select_visit(repository, "principal_roles", role_columns, 2u, "client_id", &key,
                         "position", FLOWIE_SECURITY_MAX_ROLES, repo_principal_child_visit, &roles,
                         &role_count);
  if (rc == TURBO_OK)
    rc = repo_select_visit(repository, "principal_groups", group_columns, 2u, "client_id", &key,
                           "position", FLOWIE_SECURITY_MAX_GROUPS, repo_principal_child_visit,
                           &groups, &group_count);
  if (rc == TURBO_OK)
    rc = repo_select_visit(repository, "subscriptions", subscription_columns, 6u, "client_id", &key,
                           nullptr, repository->limits.max_subscriptions_per_session,
                           repo_subscription_visit, children, &row->subscription_count);
  if (rc == TURBO_OK)
    rc = repo_select_visit(repository, "inflight", inflight_columns, 2u, "client_id", &key, nullptr,
                           repository->limits.max_inflight_per_session, repo_inflight_visit,
                           children, &row->inflight_count);
  if (rc == TURBO_OK)
    rc = repo_select_visit(repository, "deliveries", delivery_columns, 5u, "client_id", &key,
                           nullptr, repository->limits.max_inflight_per_session,
                           repo_delivery_visit, children, &row->delivery_count);
  if (rc == TURBO_OK &&
      row->inflight_count > repository->limits.max_inflight_per_session - row->delivery_count)
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK)
    rc = repo_select_visit(repository, "wills", will_columns, 7u, "client_id", &key, nullptr, 2u,
                           repo_will_visit, children, &will_count);
  if (rc == TURBO_OK && will_count > 1u) rc = TURBO_EPROTO;
  if (rc == TURBO_OK && row->has_principal) {
    row->principal.role_count = (uint32_t)role_count;
    row->principal.group_count = (uint32_t)group_count;
  }
  return rc;
}

static int repo_session_from_record(flowie_protocol_repository_t *repository,
                                    const repo_session_record *record,
                                    flowie_protocol_session_row_t *row) {
  int rc = TURBO_OK;
  row->client_id = repo_buffer_span(record->client_id);
  row->revision = record->revision;
  row->expected_revision = record->revision;
  row->session_id = record->session_id;
  row->session_generation = record->session_generation;
  row->expiry_at_epoch_seconds = record->expires_at;
  row->will_at_epoch_seconds = record->will_at;
  if (row->client_id.size == 0u || row->client_id.size > repository->limits.max_client_id_size ||
      row->revision == 0u || row->session_id == 0u || row->session_generation == 0u ||
      !flowie_mqtt_version_is_supported((flowie_mqtt_version_t)record->mqtt_version) ||
      record->keep_alive < 0 || record->keep_alive > UINT16_MAX ||
      record->expiry_interval > UINT32_MAX || record->next_packet_id < 0 ||
      record->next_packet_id > UINT16_MAX ||
      (record->has_principal != 0 && record->has_principal != 1))
    return TURBO_EPROTO;
  row->mqtt_version = (flowie_mqtt_version_t)record->mqtt_version;
  row->keep_alive = (uint16_t)record->keep_alive;
  row->session_expiry_interval = (uint32_t)record->expiry_interval;
  row->next_delivery_packet_id = (uint16_t)record->next_packet_id;
  row->has_principal = (int)record->has_principal;
  if (!row->has_principal) return TURBO_OK;
  rc = repo_copy_text(record->principal_id, row->principal.principal_id,
                      sizeof(row->principal.principal_id), true);
  if (rc == TURBO_OK)
    rc = repo_copy_text(record->principal_type, row->principal.principal_type,
                        sizeof(row->principal.principal_type), true);
  if (rc == TURBO_OK)
    rc = repo_copy_text(record->domain_id, row->principal.domain_id,
                        sizeof(row->principal.domain_id), false);
  if (rc == TURBO_OK)
    rc = repo_copy_text(record->auth_method, row->principal.auth_method,
                        sizeof(row->principal.auth_method), true);
  if (rc == TURBO_OK && (record->principal_scope < FLOWIE_SECURITY_SCOPE_SELF ||
                         record->principal_scope > FLOWIE_SECURITY_SCOPE_SYSTEM))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK) {
    row->principal.scope = (flowie_security_scope_t)record->principal_scope;
    row->principal.expires_at = record->principal_expires_at;
    row->principal.policy_version = record->policy_version;
  }
  return rc;
}

int flowie_protocol_repository_session_visit(flowie_protocol_repository_t *repository,
                                             flowie_protocol_session_visit_fn visit,
                                             void *visit_ctx) {
  static const flowie_orm_column_t columns[] = {
      {"client_id", FLOWIE_ORM_COLUMN_TEXT},
      {"revision", FLOWIE_ORM_COLUMN_UINT64},
      {"session_id", FLOWIE_ORM_COLUMN_UINT64},
      {"session_generation", FLOWIE_ORM_COLUMN_UINT64},
      {"mqtt_version", FLOWIE_ORM_COLUMN_INT64},
      {"keep_alive", FLOWIE_ORM_COLUMN_INT64},
      {"expiry_interval", FLOWIE_ORM_COLUMN_UINT64},
      {"next_packet_id", FLOWIE_ORM_COLUMN_INT64},
      {"expires_at", FLOWIE_ORM_COLUMN_UINT64},
      {"will_at", FLOWIE_ORM_COLUMN_UINT64},
      {"has_principal", FLOWIE_ORM_COLUMN_INT64},
      {"principal_id", FLOWIE_ORM_COLUMN_TEXT},
      {"principal_type", FLOWIE_ORM_COLUMN_TEXT},
      {"domain_id", FLOWIE_ORM_COLUMN_TEXT},
      {"auth_method", FLOWIE_ORM_COLUMN_TEXT},
      {"principal_scope", FLOWIE_ORM_COLUMN_INT64},
      {"principal_expires_at", FLOWIE_ORM_COLUMN_UINT64},
      {"policy_version", FLOWIE_ORM_COLUMN_UINT64},
  };
  vec_t records{};
  repo_session_collect_context collect;
  size_t rows = 0u;
  int rc = TURBO_OK;
  if (!repository || !visit) return TURBO_EINVAL;
  rc = repo_vec_status(vec_init_bytes(&records, sizeof(repo_session_record),
                                      alignof(repo_session_record),
                                      repository->limits.max_sessions));
  if (rc != TURBO_OK) return rc;
  collect.records = &records;
  rc = repo_select_visit(repository, "sessions", columns, sizeof(columns) / sizeof(columns[0]),
                         nullptr, nullptr, nullptr, repository->limits.max_sessions,
                         repo_session_collect, &collect, &rows);
  for (size_t i = 0u; rc == TURBO_OK && i < rows; ++i) {
    flowie_protocol_session_row_t row = FLOWIE_PROTOCOL_SESSION_ROW_INIT;
    repo_session_children children{};
    const auto *record = static_cast<const repo_session_record *>(vec_at_const(&records, i));
    rc = record ? repo_session_from_record(repository, record, &row) : TURBO_EIO;
    if (rc == TURBO_OK) rc = repo_session_load_children(repository, &row, &children);
    if (rc == TURBO_OK) rc = visit(visit_ctx, &row);
    repo_session_children_cleanup(&children);
  }
  for (size_t i = 0u; i < vec_size(&records); ++i)
    repo_session_record_destroy(static_cast<repo_session_record *>(vec_at(&records, i)));
  vec_destroy(&records);
  return rc;
}
