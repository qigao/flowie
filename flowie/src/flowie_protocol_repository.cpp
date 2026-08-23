#include "flowie_protocol_repository.h"

#include "orm.h"
#include "turbo_error.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

struct flowie_protocol_repository_s {
  orm_connection_t *connection;
  flowie_protocol_repository_limits_t limits;
  char prefix[64];
  bool postgres;
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
  if (blob)
    out.data.blob_value = {value.data, value.size};
  else
    out.data.text_value = {(const char *)value.data, value.size};
  return out;
}

static int repo_status(orm_status_t status) {
  switch (status) {
  case ORM_STATUS_OK:
    return TURBO_OK;
  case ORM_STATUS_INVALID_ARGUMENT:
  case ORM_STATUS_ABI_MISMATCH:
  case ORM_STATUS_TYPE_ERROR:
  case ORM_STATUS_OUT_OF_RANGE:
  case ORM_STATUS_NULL_VALUE:
  case ORM_STATUS_INVALID_STATE:
    return TURBO_EINVAL;
  case ORM_STATUS_OUT_OF_MEMORY:
    return TURBO_ENOMEM;
  case ORM_STATUS_LIMIT_EXCEEDED:
    return TURBO_ENOSPC;
  case ORM_STATUS_BUSY:
    return TURBO_EBUSY;
  case ORM_STATUS_UNSUPPORTED:
    return TURBO_ENOTSUP;
  case ORM_STATUS_CONNECTION_ERROR:
  case ORM_STATUS_SQL_ERROR:
  case ORM_STATUS_DATASTORE_ERROR:
  case ORM_STATUS_INTERNAL_ERROR:
  default:
    return TURBO_EIO;
  }
}

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
  orm_result_t *result = nullptr;
  orm_error_init(&error);
  orm_status_t status = orm_raw(repository->connection, repo_view(sql), &query, &error);
  if (status == ORM_STATUS_OK) status = orm_query_execute(query, &result, &error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return repo_status(status);
}

static int repo_create_schema(flowie_protocol_repository_t *repository) {
  const char *blob = repository->postgres ? "bytea" : "blob";
  char sql[2048];
  char name[128];
  int rc;

#define REPO_SCHEMA(table, body)                                                               \
  do {                                                                                         \
    rc = repo_table(repository, table, name, sizeof(name));                                    \
    if (rc != TURBO_OK) return rc;                                                             \
    auto format_schema = &std::snprintf;                                                       \
    int written = format_schema(sql, sizeof(sql), "create table if not exists %s(" body ")", \
                                name, blob, blob, blob, blob);                                  \
    if (written <= 0 || (size_t)written >= sizeof(sql)) return TURBO_ERANGE;                   \
    rc = repo_execute_raw(repository, sql);                                                    \
    if (rc != TURBO_OK) return rc;                                                             \
  } while (0)

  REPO_SCHEMA("meta", "schema_version integer not null");
  REPO_SCHEMA("sessions",
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
  REPO_SCHEMA("subscriptions",
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
  REPO_SCHEMA("wills",
              "client_id text primary key, pending integer not null, qos integer not null, retain "
              "integer not null, delay_interval bigint not null, topic text not null, properties %s "
              "not null, payload %s not null");
  REPO_SCHEMA("retained",
              "topic text primary key, revision bigint not null, publisher_session_id bigint not null, "
              "expires_at bigint not null, mqtt_version integer not null, qos integer not null, "
              "properties %s not null, payload %s "
              "not null");
#undef REPO_SCHEMA

  if (repo_table(repository, "meta", name, sizeof(name)) != TURBO_OK) return TURBO_ERANGE;
  std::snprintf(sql, sizeof(sql), "insert into %s(schema_version) select %u where not exists "
                                  "(select 1 from %s)",
                name, FLOWIE_PROTOCOL_REPOSITORY_SCHEMA_VERSION, name);
  return repo_execute_raw(repository, sql);
}

static int repo_validate_schema(flowie_protocol_repository_t *repository) {
  char table[128];
  char sql[256];
  orm_error_t error;
  orm_query_t *query = nullptr;
  orm_result_t *result = nullptr;
  uint64_t rows = 0u;
  int64_t version = 0;
  int rc = repo_table(repository, "meta", table, sizeof(table));
  if (rc != TURBO_OK) return rc;
  const int written = std::snprintf(sql, sizeof(sql), "select schema_version from %s", table);
  if (written <= 0 || (size_t)written >= sizeof(sql)) return TURBO_ERANGE;
  orm_error_init(&error);
  orm_status_t status = orm_raw(repository->connection, repo_view(sql), &query, &error);
  if (status == ORM_STATUS_OK) status = orm_query_execute(query, &result, &error);
  if (status == ORM_STATUS_OK) status = orm_result_row_count(result, &rows, &error);
  if (status == ORM_STATUS_OK && rows == 1u)
    status = orm_result_get_int64(result, 0u, 0u, &version, &error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  if (status != ORM_STATUS_OK) return repo_status(status);
  return rows == 1u && version == FLOWIE_PROTOCOL_REPOSITORY_SCHEMA_VERSION ? TURBO_OK
                                                                           : TURBO_EPROTO;
}

static bool repo_limits_valid(const flowie_protocol_repository_limits_t *limits) {
  return limits && limits->max_sessions != 0u && limits->max_subscriptions_per_session != 0u &&
         limits->max_inflight_per_session != 0u && limits->max_retained_messages != 0u &&
         limits->max_client_id_size != 0u && limits->max_topic_size != 0u &&
         limits->max_packet_size != 0u;
}

static bool repo_cstr_valid(const char *value, size_t capacity, bool required) {
  if (!value || capacity == 0u) return false;
  const void *end = std::memchr(value, '\0', capacity);
  return end && (!required || end != value);
}

int flowie_protocol_repository_open(const flowie_protocol_repository_config_t *config,
                                    flowie_protocol_repository_t **out) {
  flowie_protocol_repository_t *repository = nullptr;
  orm_option_t *options = nullptr;
  orm_config_t orm_settings;
  orm_error_t error;
  orm_status_t status;
  int rc = TURBO_EINVAL;
  if (out) *out = nullptr;
  if (!config || config->size < sizeof(*config) || !out || !config->driver ||
      !config->driver[0] || !repo_limits_valid(&config->limits) ||
      (config->option_count != 0u && !config->options) || config->option_count > UINT32_MAX)
    return TURBO_EINVAL;
  repository = (flowie_protocol_repository_t *)std::calloc(1u, sizeof(*repository));
  if (!repository) return TURBO_ENOMEM;
  repository->limits = config->limits;
  repository->postgres = std::strcmp(config->driver, "postgresql") == 0 ||
                         std::strcmp(config->driver, "postgres") == 0;
  const char *prefix = config->namespace_name && config->namespace_name[0]
                           ? config->namespace_name
                           : "flowie_protocol";
  size_t prefix_size = std::strlen(prefix);
  if (prefix_size > sizeof(repository->prefix) - 2u) goto done;
  for (size_t i = 0u; i < prefix_size; ++i) {
    char c = prefix[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (i != 0u && c >= '0' && c <= '9') || c == '_'))
      goto done;
  }
  std::memcpy(repository->prefix, prefix, prefix_size);
  repository->prefix[prefix_size] = '_';
  if (config->option_count != 0u) {
    options = (orm_option_t *)std::calloc(config->option_count, sizeof(*options));
    if (!options) {
      rc = TURBO_ENOMEM;
      goto done;
    }
    for (size_t i = 0u; i < config->option_count; ++i) {
      if (!config->options[i].name || !config->options[i].value) goto done;
      options[i].keyword = repo_view(config->options[i].name);
      options[i].value = repo_view(config->options[i].value);
    }
  }
  orm_config(&orm_settings);
  orm_settings.driver = repo_view(config->driver);
  orm_settings.options = options;
  orm_settings.option_count = (uint32_t)config->option_count;
  orm_settings.max_result_rows =
      config->limits.max_sessions > config->limits.max_retained_messages
          ? config->limits.max_sessions
          : config->limits.max_retained_messages;
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
  std::free(options);
  flowie_protocol_repository_close(repository);
  return rc;
}

void flowie_protocol_repository_close(flowie_protocol_repository_t *repository) {
  if (!repository) return;
  orm_disconnect(repository->connection);
  std::free(repository);
}

static int repo_query_finish(orm_query_t *query, orm_transaction_t *transaction,
                             orm_result_t **out) {
  orm_error_t error;
  orm_result_t *discard = nullptr;
  orm_error_init(&error);
  orm_status_t status = transaction
                            ? orm_query_execute_in_transaction(query, transaction,
                                                               out ? out : &discard, &error)
                            : orm_query_execute(query, out ? out : &discard, &error);
  orm_result_destroy(discard);
  orm_query_destroy(query);
  return repo_status(status);
}

static int repo_query_set(orm_query_t *query, const char *column, orm_value_t value) {
  orm_error_t error;
  orm_error_init(&error);
  return repo_status(orm_query_set(query, repo_view(column), value, &error));
}

static int repo_query_where(orm_query_t *query, const char *column, orm_value_t value) {
  orm_error_t error;
  orm_error_init(&error);
  return repo_status(
      orm_query_where(query, repo_view(column), ORM_COMPARE_EQUAL, value, &error));
}

static int repo_query_new(flowie_protocol_repository_t *repository, const char *table,
                          int kind, orm_query_t **out) {
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
  return repo_query_finish(query, transaction, nullptr);
}

static int repo_existing_revision(flowie_protocol_repository_t *repository, const char *table,
                                  const char *key_column, orm_value_t key,
                                  orm_transaction_t *transaction, bool *found,
                                  uint64_t *revision) {
  orm_query_t *query = nullptr;
  orm_result_t *result = nullptr;
  orm_error_t error;
  uint64_t rows = 0u;
  int rc;
  *found = false;
  *revision = 0u;
  rc = repo_query_new(repository, table, 0, &query);
  orm_error_init(&error);
  if (rc == TURBO_OK)
    rc = repo_status(orm_query_add_column(query, repo_view("revision"), &error));
  if (rc == TURBO_OK) rc = repo_query_where(query, key_column, key);
  if (rc == TURBO_OK) rc = repo_query_finish(query, transaction, &result);
  else
    orm_query_destroy(query);
  if (rc == TURBO_OK)
    rc = repo_status(orm_result_row_count(result, &rows, &error));
  if (rc == TURBO_OK && rows > 1u) rc = TURBO_EPROTO;
  if (rc == TURBO_OK && rows == 1u) {
    rc = repo_status(orm_result_get_uint64(result, 0u, 0u, revision, &error));
    if (rc == TURBO_OK) *found = true;
  }
  orm_result_destroy(result);
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
  orm_result_t *result = nullptr;
  int rc = repo_table(repository, table, name, sizeof(name));
  if (rc != TURBO_OK || !count) return rc != TURBO_OK ? rc : TURBO_EINVAL;
  const int written = std::snprintf(sql, sizeof(sql), "select count(*) from %s", name);
  if (written <= 0 || (size_t)written >= sizeof(sql)) return TURBO_ERANGE;
  orm_error_init(&error);
  orm_status_t status = orm_raw(repository->connection, repo_view(sql), &query, &error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute_in_transaction(query, transaction, &result, &error);
  if (status == ORM_STATUS_OK) status = orm_result_get_uint64(result, 0u, 0u, count, &error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return repo_status(status);
}

static int repo_insert_session(flowie_protocol_repository_t *repository,
                               orm_transaction_t *transaction,
                               const flowie_protocol_session_row_t *row) {
  orm_query_t *query = nullptr;
  const flowie_security_principal_t *p = &row->principal;
  int rc = repo_query_new(repository, "sessions", 1, &query);
#define REPO_SET(column, value)                                                                \
  do {                                                                                         \
    if (rc == TURBO_OK) rc = repo_query_set(query, column, value);                            \
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
  return repo_query_finish(query, transaction, nullptr);
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
  return repo_query_finish(query, transaction, nullptr);
}

static int repo_replace_session_children(flowie_protocol_repository_t *repository,
                                         orm_transaction_t *transaction,
                                         const flowie_protocol_session_row_t *row) {
  static const char *const child_tables[] = {"principal_roles", "principal_groups",
                                             "subscriptions", "inflight", "deliveries", "wills"};
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
    const char *columns[] = {"filter", "qos", "no_local", "retain_as_published",
                             "retain_handling", "subscription_identifier"};
    orm_value_t values[] = {repo_span(entry.filter, false), repo_i64(entry.qos),
                            repo_i64(entry.no_local), repo_i64(entry.retain_as_published),
                            repo_i64(entry.retain_handling),
                            repo_u64(entry.subscription_identifier)};
    rc = repo_insert_child(repository, transaction, "subscriptions", row->client_id, columns,
                           values, 6u);
    if (rc != TURBO_OK) return rc;
  }
  for (size_t i = 0u; i < row->inflight_count; ++i) {
    const char *columns[] = {"packet_id", "qos"};
    orm_value_t values[] = {repo_i64(row->inflight[i].packet_id),
                            repo_i64(row->inflight[i].qos)};
    rc = repo_insert_child(repository, transaction, "inflight", row->client_id, columns, values,
                           2u);
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
    const char *columns[] = {"pending", "qos", "retain", "delay_interval", "topic",
                             "properties", "payload"};
    orm_value_t values[] = {repo_i64(row->will.pending), repo_i64(row->will.qos),
                            repo_i64(row->will.retain), repo_u64(row->will.delay_interval),
                            repo_span(row->will.topic, false),
                            repo_span(row->will.properties, true), repo_span(row->will.payload, true)};
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
        !repo_cstr_valid(row->principal.principal_type, sizeof(row->principal.principal_type), true) ||
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
  if (rc == TURBO_OK)
    rc = repo_status(orm_transaction_commit(transaction, &error));
  else
    (void)orm_transaction_rollback(transaction, &error);
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
  if (rc == TURBO_OK)
    rc = repo_status(orm_transaction_commit(transaction, &error));
  else
    (void)orm_transaction_rollback(transaction, &error);
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
    if (rc == TURBO_OK) rc = repo_query_finish(query, transaction, nullptr);
    else
      orm_query_destroy(query);
    query = nullptr;
  }
  if (rc == TURBO_OK) rc = repo_query_new(repository, "retained", 1, &query);
#define RETAINED_SET(column, value)                                                            \
  do {                                                                                         \
    if (rc == TURBO_OK) rc = repo_query_set(query, column, value);                            \
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
  if (rc == TURBO_OK) rc = repo_query_finish(query, transaction, nullptr);
  else
    orm_query_destroy(query);
  if (rc == TURBO_OK)
    rc = repo_status(orm_transaction_commit(transaction, &error));
  else
    (void)orm_transaction_rollback(transaction, &error);
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
  if (rc == TURBO_OK) rc = repo_query_finish(query, transaction, nullptr);
  else
    orm_query_destroy(query);
  if (rc == TURBO_OK)
    rc = repo_status(orm_transaction_commit(transaction, &error));
  else
    (void)orm_transaction_rollback(transaction, &error);
  orm_transaction_destroy(transaction);
  return rc;
}

static int repo_select(flowie_protocol_repository_t *repository, const char *table,
                       const char *const *columns, size_t column_count, const char *key_column,
                       const orm_value_t *key, orm_result_t **out,
                       const char *order_column = nullptr) {
  orm_query_t *query = nullptr;
  orm_error_t error;
  int rc = repo_query_new(repository, table, 0, &query);
  orm_error_init(&error);
  for (size_t i = 0u; rc == TURBO_OK && i < column_count; ++i)
    rc = repo_status(orm_query_add_column(query, repo_view(columns[i]), &error));
  if (rc == TURBO_OK && key_column && key) rc = repo_query_where(query, key_column, *key);
  if (rc == TURBO_OK && order_column)
    rc = repo_status(
        orm_query_order_by(query, repo_view(order_column), ORM_ORDER_ASCENDING, &error));
  if (rc == TURBO_OK) return repo_query_finish(query, nullptr, out);
  orm_query_destroy(query);
  return rc;
}

static int repo_rows(const orm_result_t *result, uint64_t *rows) {
  orm_error_t error;
  orm_error_init(&error);
  return repo_status(orm_result_row_count(result, rows, &error));
}

static int repo_get_u64(const orm_result_t *result, uint64_t row, uint64_t column,
                        uint64_t *out) {
  orm_error_t error;
  orm_error_init(&error);
  return repo_status(orm_result_get_uint64(result, row, column, out, &error));
}

static int repo_get_i64(const orm_result_t *result, uint64_t row, uint64_t column,
                        int64_t *out) {
  orm_error_t error;
  orm_error_init(&error);
  return repo_status(orm_result_get_int64(result, row, column, out, &error));
}

static int repo_get_text(const orm_result_t *result, uint64_t row, uint64_t column,
                         flowie_mqtt_span_t *out) {
  orm_error_t error;
  orm_string_view_t view{};
  orm_error_init(&error);
  int rc = repo_status(orm_result_get_text(result, row, column, &view, &error));
  if (rc == TURBO_OK) *out = {(const uint8_t *)view.data, view.len};
  return rc;
}

static int repo_get_blob(const orm_result_t *result, uint64_t row, uint64_t column,
                         flowie_mqtt_span_t *out) {
  orm_error_t error;
  orm_blob_t view{};
  orm_error_init(&error);
  int rc = repo_status(orm_result_get_blob(result, row, column, &view, &error));
  if (rc == TURBO_OK) *out = {(const uint8_t *)view.data, view.size};
  return rc;
}

int flowie_protocol_repository_retained_visit(flowie_protocol_repository_t *repository,
                                              flowie_protocol_retained_visit_fn visit,
                                              void *visit_ctx) {
  static const char *const columns[] = {"topic", "revision", "publisher_session_id",
                                        "expires_at", "mqtt_version", "qos", "properties",
                                        "payload"};
  orm_result_t *result = nullptr;
  uint64_t rows = 0u;
  int rc;
  if (!repository || !visit) return TURBO_EINVAL;
  rc = repo_select(repository, "retained", columns, 8u, nullptr, nullptr, &result);
  if (rc == TURBO_OK) rc = repo_rows(result, &rows);
  if (rc == TURBO_OK && rows > repository->limits.max_retained_messages) rc = TURBO_EPROTO;
  for (uint64_t i = 0u; rc == TURBO_OK && i < rows; ++i) {
    flowie_protocol_retained_row_t row = FLOWIE_PROTOCOL_RETAINED_ROW_INIT;
    int64_t mqtt_version = 0;
    int64_t qos = 0;
    rc = repo_get_text(result, i, 0u, &row.topic);
    if (rc == TURBO_OK) rc = repo_get_u64(result, i, 1u, &row.revision);
    if (rc == TURBO_OK) rc = repo_get_u64(result, i, 2u, &row.publisher_session_id);
    if (rc == TURBO_OK) rc = repo_get_u64(result, i, 3u, &row.expiry_at_epoch_seconds);
    if (rc == TURBO_OK) rc = repo_get_i64(result, i, 4u, &mqtt_version);
    if (rc == TURBO_OK) rc = repo_get_i64(result, i, 5u, &qos);
    if (rc == TURBO_OK) rc = repo_get_blob(result, i, 6u, &row.properties);
    if (rc == TURBO_OK) rc = repo_get_blob(result, i, 7u, &row.payload);
    row.expected_revision = row.revision;
    if (rc == TURBO_OK &&
        (qos < 0 || qos > 2 ||
         !flowie_mqtt_version_is_supported((flowie_mqtt_version_t)mqtt_version) ||
         !row.topic.data || row.topic.size == 0u ||
         row.topic.size > repository->limits.max_topic_size || row.revision == 0u ||
         row.properties.size > repository->limits.max_packet_size ||
         row.payload.size > repository->limits.max_packet_size))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) {
      row.mqtt_version = (flowie_mqtt_version_t)mqtt_version;
      row.qos = (uint8_t)qos;
      rc = visit(visit_ctx, &row);
    }
  }
  orm_result_destroy(result);
  return rc;
}

static int repo_copy_text(const orm_result_t *result, uint64_t row, uint64_t column, char *out,
                          size_t capacity, bool required) {
  flowie_mqtt_span_t value{};
  int rc = repo_get_text(result, row, column, &value);
  if (rc != TURBO_OK) return rc;
  if (value.size >= capacity || (required && value.size == 0u) ||
      (value.size != 0u && std::memchr(value.data, '\0', value.size)))
    return TURBO_EPROTO;
  if (value.size != 0u) std::memcpy(out, value.data, value.size);
  out[value.size] = '\0';
  return TURBO_OK;
}

static int repo_session_child_results(flowie_protocol_repository_t *repository,
                                      flowie_mqtt_span_t client_id, orm_result_t **roles,
                                      orm_result_t **groups, orm_result_t **subscriptions,
                                      orm_result_t **inflight, orm_result_t **deliveries,
                                      orm_result_t **will) {
  const orm_value_t key = repo_span(client_id, false);
  static const char *const role_columns[] = {"position", "role"};
  static const char *const group_columns[] = {"position", "group_id"};
  static const char *const subscription_columns[] = {
      "filter", "qos", "no_local", "retain_as_published", "retain_handling",
      "subscription_identifier"};
  static const char *const inflight_columns[] = {"packet_id", "qos"};
  static const char *const delivery_columns[] = {"packet_id", "qos", "state", "expires_at",
                                                  "packet"};
  static const char *const will_columns[] = {"pending", "qos", "retain", "delay_interval",
                                              "topic", "properties", "payload"};
  int rc = repo_select(repository, "principal_roles", role_columns, 2u, "client_id", &key, roles,
                       "position");
  if (rc == TURBO_OK)
    rc = repo_select(repository, "principal_groups", group_columns, 2u, "client_id", &key,
                     groups, "position");
  if (rc == TURBO_OK)
    rc = repo_select(repository, "subscriptions", subscription_columns, 6u, "client_id", &key,
                     subscriptions);
  if (rc == TURBO_OK)
    rc = repo_select(repository, "inflight", inflight_columns, 2u, "client_id", &key, inflight);
  if (rc == TURBO_OK)
    rc = repo_select(repository, "deliveries", delivery_columns, 5u, "client_id", &key,
                     deliveries);
  if (rc == TURBO_OK)
    rc = repo_select(repository, "wills", will_columns, 7u, "client_id", &key, will);
  return rc;
}

static int repo_session_read_principal(const orm_result_t *sessions, uint64_t session_row,
                                       orm_result_t *roles, orm_result_t *groups,
                                       flowie_protocol_session_row_t *out) {
  uint64_t role_rows = 0u;
  uint64_t group_rows = 0u;
  int64_t flag = 0;
  int64_t scope = 0;
  int rc = repo_get_i64(sessions, session_row, 10u, &flag);
  if (rc != TURBO_OK || (flag != 0 && flag != 1)) return TURBO_EPROTO;
  out->has_principal = (int)flag;
  if (!out->has_principal) return TURBO_OK;
  rc = repo_copy_text(sessions, session_row, 11u, out->principal.principal_id,
                      sizeof(out->principal.principal_id), true);
  if (rc == TURBO_OK)
    rc = repo_copy_text(sessions, session_row, 12u, out->principal.principal_type,
                        sizeof(out->principal.principal_type), true);
  if (rc == TURBO_OK)
    rc = repo_copy_text(sessions, session_row, 13u, out->principal.domain_id,
                        sizeof(out->principal.domain_id), false);
  if (rc == TURBO_OK)
    rc = repo_copy_text(sessions, session_row, 14u, out->principal.auth_method,
                        sizeof(out->principal.auth_method), true);
  if (rc == TURBO_OK) rc = repo_get_i64(sessions, session_row, 15u, &scope);
  if (rc == TURBO_OK)
    rc = repo_get_u64(sessions, session_row, 16u, &out->principal.expires_at);
  if (rc == TURBO_OK)
    rc = repo_get_u64(sessions, session_row, 17u, &out->principal.policy_version);
  if (rc == TURBO_OK) rc = repo_rows(roles, &role_rows);
  if (rc == TURBO_OK) rc = repo_rows(groups, &group_rows);
  if (rc != TURBO_OK || scope < FLOWIE_SECURITY_SCOPE_SELF ||
      scope > FLOWIE_SECURITY_SCOPE_SYSTEM || role_rows > FLOWIE_SECURITY_MAX_ROLES ||
      group_rows > FLOWIE_SECURITY_MAX_GROUPS)
    return TURBO_EPROTO;
  out->principal.scope = (flowie_security_scope_t)scope;
  out->principal.role_count = (uint32_t)role_rows;
  out->principal.group_count = (uint32_t)group_rows;
  for (uint64_t i = 0u; rc == TURBO_OK && i < role_rows; ++i) {
    int64_t position = -1;
    rc = repo_get_i64(roles, i, 0u, &position);
    if (rc == TURBO_OK && position != (int64_t)i) rc = TURBO_EPROTO;
    if (rc == TURBO_OK)
      rc = repo_copy_text(roles, i, 1u, out->principal.roles[i],
                          sizeof(out->principal.roles[i]), true);
  }
  for (uint64_t i = 0u; rc == TURBO_OK && i < group_rows; ++i) {
    int64_t position = -1;
    rc = repo_get_i64(groups, i, 0u, &position);
    if (rc == TURBO_OK && position != (int64_t)i) rc = TURBO_EPROTO;
    if (rc == TURBO_OK)
      rc = repo_copy_text(groups, i, 1u, out->principal.groups[i],
                          sizeof(out->principal.groups[i]), true);
  }
  return rc;
}

static int repo_session_read_children(flowie_protocol_repository_t *repository,
                                      orm_result_t *subscriptions, orm_result_t *inflight,
                                      orm_result_t *deliveries, orm_result_t *will,
                                      flowie_protocol_session_row_t *out) {
  uint64_t subscription_rows = 0u;
  uint64_t inflight_rows = 0u;
  uint64_t delivery_rows = 0u;
  uint64_t will_rows = 0u;
  int rc = repo_rows(subscriptions, &subscription_rows);
  if (rc == TURBO_OK) rc = repo_rows(inflight, &inflight_rows);
  if (rc == TURBO_OK) rc = repo_rows(deliveries, &delivery_rows);
  if (rc == TURBO_OK) rc = repo_rows(will, &will_rows);
  if (rc != TURBO_OK || subscription_rows > repository->limits.max_subscriptions_per_session ||
      inflight_rows + delivery_rows > repository->limits.max_inflight_per_session || will_rows > 1u)
    return TURBO_EPROTO;
  auto *subscription_values = (flowie_protocol_subscription_row_t *)std::calloc(
      (size_t)subscription_rows, sizeof(flowie_protocol_subscription_row_t));
  auto *inflight_values = (flowie_protocol_inflight_row_t *)std::calloc(
      (size_t)inflight_rows, sizeof(flowie_protocol_inflight_row_t));
  auto *delivery_values = (flowie_protocol_delivery_row_t *)std::calloc(
      (size_t)delivery_rows, sizeof(flowie_protocol_delivery_row_t));
  if ((subscription_rows && !subscription_values) || (inflight_rows && !inflight_values) ||
      (delivery_rows && !delivery_values)) {
    std::free(subscription_values);
    std::free(inflight_values);
    std::free(delivery_values);
    return TURBO_ENOMEM;
  }
  out->subscriptions = subscription_values;
  out->subscription_count = (size_t)subscription_rows;
  out->inflight = inflight_values;
  out->inflight_count = (size_t)inflight_rows;
  out->deliveries = delivery_values;
  out->delivery_count = (size_t)delivery_rows;
  for (uint64_t i = 0u; rc == TURBO_OK && i < subscription_rows; ++i) {
    int64_t qos = 0, no_local = 0, rap = 0, handling = 0;
    uint64_t identifier = 0u;
    rc = repo_get_text(subscriptions, i, 0u, &subscription_values[i].filter);
    if (rc == TURBO_OK) rc = repo_get_i64(subscriptions, i, 1u, &qos);
    if (rc == TURBO_OK) rc = repo_get_i64(subscriptions, i, 2u, &no_local);
    if (rc == TURBO_OK) rc = repo_get_i64(subscriptions, i, 3u, &rap);
    if (rc == TURBO_OK) rc = repo_get_i64(subscriptions, i, 4u, &handling);
    if (rc == TURBO_OK) rc = repo_get_u64(subscriptions, i, 5u, &identifier);
    if (rc == TURBO_OK && (qos < 0 || qos > 2 || no_local < 0 || no_local > 1 || rap < 0 ||
                           rap > 1 || handling < 0 || handling > 2 || identifier > UINT32_MAX))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) {
      subscription_values[i].qos = (uint8_t)qos;
      subscription_values[i].no_local = (uint8_t)no_local;
      subscription_values[i].retain_as_published = (uint8_t)rap;
      subscription_values[i].retain_handling = (uint8_t)handling;
      subscription_values[i].subscription_identifier = (uint32_t)identifier;
    }
  }
  for (uint64_t i = 0u; rc == TURBO_OK && i < inflight_rows; ++i) {
    int64_t packet_id = 0, qos = 0;
    rc = repo_get_i64(inflight, i, 0u, &packet_id);
    if (rc == TURBO_OK) rc = repo_get_i64(inflight, i, 1u, &qos);
    if (rc == TURBO_OK && (packet_id <= 0 || packet_id > UINT16_MAX || qos < 0 || qos > 2))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) {
      inflight_values[i].packet_id = (uint16_t)packet_id;
      inflight_values[i].qos = (uint8_t)qos;
    }
  }
  for (uint64_t i = 0u; rc == TURBO_OK && i < delivery_rows; ++i) {
    int64_t packet_id = 0, qos = 0, state = 0;
    rc = repo_get_i64(deliveries, i, 0u, &packet_id);
    if (rc == TURBO_OK) rc = repo_get_i64(deliveries, i, 1u, &qos);
    if (rc == TURBO_OK) rc = repo_get_i64(deliveries, i, 2u, &state);
    if (rc == TURBO_OK)
      rc = repo_get_u64(deliveries, i, 3u, &delivery_values[i].expiry_at_epoch_seconds);
    if (rc == TURBO_OK) rc = repo_get_blob(deliveries, i, 4u, &delivery_values[i].packet);
    if (rc == TURBO_OK &&
        (packet_id <= 0 || packet_id > UINT16_MAX || qos < 0 || qos > 2 || state <= 0 ||
         state > UINT8_MAX || delivery_values[i].packet.size > repository->limits.max_packet_size))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) {
      delivery_values[i].packet_id = (uint16_t)packet_id;
      delivery_values[i].qos = (uint8_t)qos;
      delivery_values[i].state = (uint8_t)state;
    }
  }
  if (rc == TURBO_OK && will_rows == 1u) {
    int64_t pending = 0, qos = 0, retain = 0;
    uint64_t delay = 0u;
    out->will.present = 1;
    rc = repo_get_i64(will, 0u, 0u, &pending);
    if (rc == TURBO_OK) rc = repo_get_i64(will, 0u, 1u, &qos);
    if (rc == TURBO_OK) rc = repo_get_i64(will, 0u, 2u, &retain);
    if (rc == TURBO_OK) rc = repo_get_u64(will, 0u, 3u, &delay);
    if (rc == TURBO_OK) rc = repo_get_text(will, 0u, 4u, &out->will.topic);
    if (rc == TURBO_OK) rc = repo_get_blob(will, 0u, 5u, &out->will.properties);
    if (rc == TURBO_OK) rc = repo_get_blob(will, 0u, 6u, &out->will.payload);
    if (rc == TURBO_OK && (pending < 0 || pending > 1 || qos < 0 || qos > 2 || retain < 0 ||
                           retain > 1 || delay > UINT32_MAX))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) {
      out->will.pending = (int)pending;
      out->will.qos = (uint8_t)qos;
      out->will.retain = (uint8_t)retain;
      out->will.delay_interval = (uint32_t)delay;
    }
  }
  return rc;
}

static void repo_session_children_cleanup(flowie_protocol_session_row_t *row) {
  std::free((void *)row->subscriptions);
  std::free((void *)row->inflight);
  std::free((void *)row->deliveries);
  row->subscriptions = nullptr;
  row->inflight = nullptr;
  row->deliveries = nullptr;
}

int flowie_protocol_repository_session_visit(flowie_protocol_repository_t *repository,
                                             flowie_protocol_session_visit_fn visit,
                                             void *visit_ctx) {
  static const char *const columns[] = {
      "client_id",          "revision",          "session_id",       "session_generation",
      "mqtt_version",       "keep_alive",        "expiry_interval",  "next_packet_id",
      "expires_at",         "will_at",           "has_principal",    "principal_id",
      "principal_type",     "domain_id",         "auth_method",      "principal_scope",
      "principal_expires_at", "policy_version"};
  orm_result_t *sessions = nullptr;
  uint64_t rows = 0u;
  int rc;
  if (!repository || !visit) return TURBO_EINVAL;
  rc = repo_select(repository, "sessions", columns, 18u, nullptr, nullptr, &sessions);
  if (rc == TURBO_OK) rc = repo_rows(sessions, &rows);
  if (rc == TURBO_OK && rows > repository->limits.max_sessions) rc = TURBO_EPROTO;
  for (uint64_t i = 0u; rc == TURBO_OK && i < rows; ++i) {
    orm_result_t *children[6] = {};
    flowie_protocol_session_row_t row = FLOWIE_PROTOCOL_SESSION_ROW_INIT;
    int64_t mqtt_version = 0, keep_alive = 0, next_packet_id = 0;
    uint64_t expiry_interval = 0u;
    rc = repo_get_text(sessions, i, 0u, &row.client_id);
    if (rc == TURBO_OK) rc = repo_get_u64(sessions, i, 1u, &row.revision);
    row.expected_revision = row.revision;
    if (rc == TURBO_OK) rc = repo_get_u64(sessions, i, 2u, &row.session_id);
    if (rc == TURBO_OK) rc = repo_get_u64(sessions, i, 3u, &row.session_generation);
    if (rc == TURBO_OK) rc = repo_get_i64(sessions, i, 4u, &mqtt_version);
    if (rc == TURBO_OK) rc = repo_get_i64(sessions, i, 5u, &keep_alive);
    if (rc == TURBO_OK) rc = repo_get_u64(sessions, i, 6u, &expiry_interval);
    if (rc == TURBO_OK) rc = repo_get_i64(sessions, i, 7u, &next_packet_id);
    if (rc == TURBO_OK) rc = repo_get_u64(sessions, i, 8u, &row.expiry_at_epoch_seconds);
    if (rc == TURBO_OK) rc = repo_get_u64(sessions, i, 9u, &row.will_at_epoch_seconds);
    if (rc == TURBO_OK &&
        (row.client_id.size == 0u || row.client_id.size > repository->limits.max_client_id_size ||
         row.revision == 0u || row.session_id == 0u || row.session_generation == 0u ||
         !flowie_mqtt_version_is_supported((flowie_mqtt_version_t)mqtt_version) ||
         keep_alive < 0 || keep_alive > UINT16_MAX || expiry_interval > UINT32_MAX ||
         next_packet_id < 0 || next_packet_id > UINT16_MAX))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) {
      row.mqtt_version = (flowie_mqtt_version_t)mqtt_version;
      row.keep_alive = (uint16_t)keep_alive;
      row.session_expiry_interval = (uint32_t)expiry_interval;
      row.next_delivery_packet_id = (uint16_t)next_packet_id;
      rc = repo_session_child_results(repository, row.client_id, &children[0], &children[1],
                                      &children[2], &children[3], &children[4], &children[5]);
    }
    if (rc == TURBO_OK)
      rc = repo_session_read_principal(sessions, i, children[0], children[1], &row);
    if (rc == TURBO_OK)
      rc = repo_session_read_children(repository, children[2], children[3], children[4],
                                      children[5], &row);
    if (rc == TURBO_OK) rc = visit(visit_ctx, &row);
    repo_session_children_cleanup(&row);
    for (orm_result_t *child : children) orm_result_destroy(child);
  }
  orm_result_destroy(sessions);
  return rc;
}
