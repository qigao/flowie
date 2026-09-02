#include "flowie_control_data_transfer_internal.h"

#include "flowie_control_acl_internal.h"
#include "flowie_control_data_schema_internal.h"
#include "flowie_control_database_internal.h"
#include "flowie_control_management_service_internal.h"

#include "turbo_error.h"
#include "turbo_fs.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define FLOWIE_CONTROL_DATA_PATH_MAX 4095u
#define FLOWIE_CONTROL_DATA_RULE_TEXT_MAX 16383u
#define FLOWIE_CONTROL_DATA_POLICY_PAGE_MAX 8u

typedef struct flowie_control_data_database_config_s {
  orm_config_t config;
  orm_option_t options[2];
} flowie_control_data_database_config_t;

static int data_db_status(int status) {
  const int primary = status & 0xff;
  if (primary == FLOWIE_CONTROL_DB_BUSY || primary == FLOWIE_CONTROL_DB_LOCKED) return TURBO_EBUSY;
  if (primary == FLOWIE_CONTROL_DB_NOMEM) return TURBO_ENOMEM;
  if (primary == FLOWIE_CONTROL_DB_CONSTRAINT || primary == FLOWIE_CONTROL_DB_MISMATCH ||
      primary == FLOWIE_CONTROL_DB_RANGE)
    return TURBO_EINVAL;
  return TURBO_EIO;
}

static int data_db_open(const char *path, const char *mode,
                        flowie_control_database_t **database_out) {
  flowie_control_data_database_config_t database;
  int status;
  if (!path || !path[0] || !mode || !database_out) return TURBO_EINVAL;
  *database_out = NULL;
  orm_config(&database.config);
  database.options[0].keyword = orm_view("filename");
  database.options[0].value = orm_view(path);
  database.options[1].keyword = orm_view("open_mode");
  database.options[1].value = orm_view(mode);
  database.config.driver = orm_view("sqlite");
  database.config.options = database.options;
  database.config.option_count = 2u;
  status = flowie_control_database_open(&database.config, database_out);
  return status == FLOWIE_CONTROL_DB_OK ? TURBO_OK : data_db_status(status);
}

static int data_db_exec(flowie_control_database_t *database, const char *sql) {
  const int status = flowie_control_database_exec(database, sql, NULL, NULL, NULL);
  return status == FLOWIE_CONTROL_DB_OK ? TURBO_OK : data_db_status(status);
}

static int data_bind_text(flowie_control_statement_t *statement, int index, const char *value) {
  const int status =
      flowie_control_database_bind_text(statement, index, value, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  return status == FLOWIE_CONTROL_DB_OK ? TURBO_OK : data_db_status(status);
}

static int data_statement_done(flowie_control_statement_t *statement) {
  const int status = flowie_control_database_step(statement);
  if (status != FLOWIE_CONTROL_DB_DONE) return data_db_status(status);
  return flowie_control_database_reset(statement) == FLOWIE_CONTROL_DB_OK &&
                 flowie_control_database_clear_bindings(statement) == FLOWIE_CONTROL_DB_OK
             ? TURBO_OK
             : TURBO_EIO;
}

static int data_prepare(flowie_control_database_t *database, const char *sql,
                        flowie_control_statement_t **statement_out) {
  const int status = flowie_control_database_prepare(database, sql, -1, statement_out, NULL);
  return status == FLOWIE_CONTROL_DB_OK ? TURBO_OK : data_db_status(status);
}

static int data_column_text(flowie_control_statement_t *statement, int column, char *output,
                            size_t capacity, int nullable) {
  const unsigned char *text;
  int size;
  if (!statement || !output || capacity == 0u) return TURBO_EINVAL;
  output[0] = '\0';
  if (flowie_control_database_column_type(statement, column) == FLOWIE_CONTROL_DB_NULL)
    return nullable ? TURBO_OK : TURBO_EPROTO;
  if (flowie_control_database_column_type(statement, column) != FLOWIE_CONTROL_DB_TEXT ||
      (size = flowie_control_database_column_bytes(statement, column)) <= 0 ||
      (size_t)size >= capacity || !(text = flowie_control_database_column_text(statement, column)))
    return TURBO_EPROTO;
  memcpy(output, text, (size_t)size);
  output[size] = '\0';
  return TURBO_OK;
}

static int data_source_shape_preflight(flowie_control_database_t *database) {
  static const char *const queries[] = {
      "SELECT 1 FROM flowie_control_data_metadata WHERE "
      "(SELECT COUNT(*) FROM flowie_control_data_metadata)<>1 OR singleton<>1 OR "
      "format_version<>1 LIMIT 1",
      "SELECT 1 FROM flowie_control_data_user u CROSS JOIN flowie_control_data_domain d WHERE "
      "u.domain_id<>d.domain_id OR typeof(u.principal_id)<>'text' OR "
      "length(u.principal_id) NOT BETWEEN 1 AND 255 OR typeof(u.principal_type)<>'text' OR "
      "length(u.principal_type) NOT BETWEEN 1 AND 63 OR typeof(u.enabled)<>'integer' OR "
      "u.enabled NOT IN (0,1) LIMIT 1",
      "SELECT 1 FROM flowie_control_data_group g CROSS JOIN flowie_control_data_domain d "
      "LEFT JOIN flowie_control_data_group p ON p.domain_id=g.domain_id AND "
      "p.group_id=g.parent_group_id WHERE g.domain_id<>d.domain_id OR "
      "typeof(g.group_id)<>'text' OR length(g.group_id) NOT BETWEEN 1 AND 255 OR "
      "(typeof(g.parent_group_id) NOT IN ('null','text')) OR "
      "(g.parent_group_id IS NULL AND g.depth<>0) OR "
      "(g.parent_group_id IS NOT NULL AND (length(g.parent_group_id) NOT BETWEEN 1 AND 255 OR "
      "g.depth NOT BETWEEN 1 AND 15 OR p.group_id IS NULL OR p.depth<>g.depth-1)) LIMIT 1",
      "SELECT 1 FROM flowie_control_data_membership m CROSS JOIN flowie_control_data_domain d "
      "LEFT JOIN flowie_control_data_user u ON u.domain_id=m.domain_id AND "
      "u.principal_id=m.principal_id LEFT JOIN flowie_control_data_group g ON "
      "g.domain_id=m.domain_id AND g.group_id=m.group_id WHERE m.domain_id<>d.domain_id OR "
      "u.principal_id IS NULL OR g.group_id IS NULL LIMIT 1",
      "SELECT 1 FROM flowie_control_data_role r CROSS JOIN flowie_control_data_domain d WHERE "
      "r.domain_id<>d.domain_id OR typeof(r.role_id)<>'text' OR "
      "length(r.role_id) NOT BETWEEN 1 AND 63 OR typeof(r.enabled)<>'integer' OR "
      "r.enabled NOT IN (0,1) LIMIT 1",
      "SELECT 1 FROM flowie_control_data_user_role a CROSS JOIN flowie_control_data_domain d "
      "LEFT JOIN flowie_control_data_user u ON u.domain_id=a.domain_id AND "
      "u.principal_id=a.principal_id LEFT JOIN flowie_control_data_role r ON "
      "r.domain_id=a.domain_id AND r.role_id=a.role_id WHERE a.domain_id<>d.domain_id OR "
      "u.principal_id IS NULL OR r.role_id IS NULL LIMIT 1",
      "SELECT 1 FROM flowie_control_data_policy_rule p CROSS JOIN flowie_control_data_domain d "
      "LEFT JOIN flowie_control_data_user u ON p.subject_kind=1 AND u.domain_id=p.domain_id AND "
      "u.principal_id=p.subject_id LEFT JOIN flowie_control_data_role r ON p.subject_kind=2 AND "
      "r.domain_id=p.domain_id AND r.role_id=p.subject_id LEFT JOIN "
      "flowie_control_data_group g ON p.subject_kind=3 AND g.domain_id=p.domain_id AND "
      "g.group_id=p.subject_id WHERE p.domain_id<>d.domain_id OR "
      "typeof(p.subject_kind)<>'integer' OR p.subject_kind NOT IN (1,2,3) OR "
      "typeof(p.subject_id)<>'text' OR length(p.subject_id) NOT BETWEEN 1 AND 255 OR "
      "typeof(p.ordinal)<>'integer' OR p.ordinal NOT BETWEEN 0 AND 4095 OR "
      "typeof(p.rule_document)<>'text' OR length(p.rule_document) NOT BETWEEN 1 AND 16383 OR "
      "(p.subject_kind=1 AND u.principal_id IS NULL) OR "
      "(p.subject_kind=2 AND r.role_id IS NULL) OR "
      "(p.subject_kind=3 AND g.group_id IS NULL) LIMIT 1",
      "SELECT 1 FROM ("
      "SELECT principal_id AS a,'' AS b,'' AS c,COUNT(*) AS n FROM flowie_control_data_user "
      "GROUP BY domain_id,principal_id HAVING COUNT(*)>1 UNION ALL "
      "SELECT group_id,'','',COUNT(*) FROM flowie_control_data_group "
      "GROUP BY domain_id,group_id HAVING COUNT(*)>1 UNION ALL "
      "SELECT principal_id,group_id,'',COUNT(*) FROM flowie_control_data_membership "
      "GROUP BY domain_id,principal_id,group_id HAVING COUNT(*)>1 UNION ALL "
      "SELECT role_id,'','',COUNT(*) FROM flowie_control_data_role "
      "GROUP BY domain_id,role_id HAVING COUNT(*)>1 UNION ALL "
      "SELECT principal_id,role_id,'',COUNT(*) FROM flowie_control_data_user_role "
      "GROUP BY domain_id,principal_id,role_id HAVING COUNT(*)>1 UNION ALL "
      "SELECT CAST(subject_kind AS TEXT),subject_id,'',COUNT(*) "
      "FROM flowie_control_data_policy_rule GROUP BY domain_id,subject_kind,subject_id "
      "HAVING COUNT(*)>1 UNION ALL "
      "SELECT CAST(ordinal AS TEXT),'','',COUNT(*) FROM flowie_control_data_policy_rule "
      "GROUP BY domain_id,ordinal HAVING COUNT(*)>1) LIMIT 1",
      "SELECT 1 FROM (SELECT (SELECT COUNT(*) FROM flowie_control_data_user) users, "
      "(SELECT COUNT(*) FROM flowie_control_data_group) groups, "
      "(SELECT COUNT(*) FROM flowie_control_data_membership) memberships, "
      "(SELECT COUNT(*) FROM flowie_control_data_role) roles, "
      "(SELECT COUNT(*) FROM flowie_control_data_user_role) assignments, "
      "(SELECT COUNT(*) FROM flowie_control_data_policy_rule) rules) WHERE users>65536 OR "
      "groups>65536 OR memberships>1048576 OR roles>65536 OR assignments>1048576 OR rules>4096"};
  for (size_t index = 0u; index < sizeof(queries) / sizeof(queries[0]); ++index) {
    flowie_control_statement_t *statement = NULL;
    int rc = data_prepare(database, queries[index], &statement);
    if (rc == TURBO_OK) {
      const int status = flowie_control_database_step(statement);
      rc = status == FLOWIE_CONTROL_DB_DONE
               ? TURBO_OK
               : (status == FLOWIE_CONTROL_DB_ROW ? TURBO_EPROTO : data_db_status(status));
    }
    if (statement) (void)flowie_control_database_finalize(statement);
    if (rc != TURBO_OK) return rc;
  }
  return TURBO_OK;
}

static int data_insert_user(flowie_control_statement_t *statement,
                            const flowie_control_user_view_t *item) {
  int rc = data_bind_text(statement, 1, item->domain_id);
  if (rc == TURBO_OK) rc = data_bind_text(statement, 2, item->principal_id);
  if (rc == TURBO_OK) rc = data_bind_text(statement, 3, item->principal_type);
  if (rc == TURBO_OK &&
      flowie_control_database_bind_int(statement, 4, item->enabled) != FLOWIE_CONTROL_DB_OK)
    rc = TURBO_EIO;
  return rc == TURBO_OK ? data_statement_done(statement) : rc;
}

static int data_insert_group(flowie_control_statement_t *statement,
                             const flowie_control_group_view_t *item) {
  int rc = data_bind_text(statement, 1, item->domain_id);
  if (rc == TURBO_OK) rc = data_bind_text(statement, 2, item->group_id);
  if (rc == TURBO_OK) {
    const int status = item->parent_group_id[0]
                           ? flowie_control_database_bind_text(statement, 3, item->parent_group_id,
                                                               -1, FLOWIE_CONTROL_DB_TRANSIENT)
                           : flowie_control_database_bind_null(statement, 3);
    if (status != FLOWIE_CONTROL_DB_OK) rc = data_db_status(status);
  }
  if (rc == TURBO_OK && flowie_control_database_bind_int64(statement, 4, (int64_t)item->depth) !=
                            FLOWIE_CONTROL_DB_OK)
    rc = TURBO_EIO;
  return rc == TURBO_OK ? data_statement_done(statement) : rc;
}

static int data_insert_relation(flowie_control_statement_t *statement, const char *domain_id,
                                const char *first, const char *second) {
  int rc = data_bind_text(statement, 1, domain_id);
  if (rc == TURBO_OK) rc = data_bind_text(statement, 2, first);
  if (rc == TURBO_OK) rc = data_bind_text(statement, 3, second);
  return rc == TURBO_OK ? data_statement_done(statement) : rc;
}

static int data_insert_role(flowie_control_statement_t *statement,
                            const flowie_control_role_view_t *item) {
  int rc = data_bind_text(statement, 1, item->domain_id);
  if (rc == TURBO_OK) rc = data_bind_text(statement, 2, item->role_id);
  if (rc == TURBO_OK &&
      flowie_control_database_bind_int(statement, 3, item->enabled) != FLOWIE_CONTROL_DB_OK)
    rc = TURBO_EIO;
  return rc == TURBO_OK ? data_statement_done(statement) : rc;
}

static int data_export_users(const flowie_control_repository_t *repository, const char *domain_id,
                             flowie_control_database_t *database,
                             flowie_control_data_transfer_result_t *result) {
  flowie_control_user_view_t items[FLOWIE_CONTROL_PAGE_MAX];
  flowie_control_statement_t *statement = NULL;
  char after[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  int more = 0;
  int rc = data_prepare(database,
                        "INSERT INTO flowie_control_data_user(domain_id,principal_id,"
                        "principal_type,enabled) VALUES(?1,?2,?3,?4)",
                        &statement);
  do {
    size_t count = 0u;
    for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
      items[index] = (flowie_control_user_view_t)FLOWIE_CONTROL_USER_VIEW_INIT;
    if (rc == TURBO_OK)
      rc = repository->user->list(repository->ctx, domain_id, after[0] ? after : NULL, items,
                                  FLOWIE_CONTROL_PAGE_MAX, &count, &more);
    for (size_t index = 0u; rc == TURBO_OK && index < count; ++index) {
      rc = data_insert_user(statement, &items[index]);
      if (rc == TURBO_OK) ++result->user_count;
    }
    if (rc == TURBO_OK && count != 0u)
      (void)snprintf(after, sizeof(after), "%s", items[count - 1u].principal_id);
  } while (rc == TURBO_OK && more);
  if (statement) (void)flowie_control_database_finalize(statement);
  return rc;
}

static int data_export_groups(const flowie_control_repository_t *repository, const char *domain_id,
                              flowie_control_database_t *database,
                              flowie_control_data_transfer_result_t *result) {
  flowie_control_group_view_t items[FLOWIE_CONTROL_PAGE_MAX];
  flowie_control_statement_t *statement = NULL;
  char after[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  int more = 0;
  int rc = data_prepare(database,
                        "INSERT INTO flowie_control_data_group(domain_id,group_id,parent_group_id,"
                        "depth) VALUES(?1,?2,?3,?4)",
                        &statement);
  do {
    size_t count = 0u;
    for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
      items[index] = (flowie_control_group_view_t)FLOWIE_CONTROL_GROUP_VIEW_INIT;
    if (rc == TURBO_OK)
      rc = repository->group->list(repository->ctx, domain_id, after[0] ? after : NULL, items,
                                   FLOWIE_CONTROL_PAGE_MAX, &count, &more);
    for (size_t index = 0u; rc == TURBO_OK && index < count; ++index) {
      rc = data_insert_group(statement, &items[index]);
      if (rc == TURBO_OK) ++result->group_count;
    }
    if (rc == TURBO_OK && count != 0u)
      (void)snprintf(after, sizeof(after), "%s", items[count - 1u].group_id);
  } while (rc == TURBO_OK && more);
  if (statement) (void)flowie_control_database_finalize(statement);
  return rc;
}

static int data_export_memberships(const flowie_control_repository_t *repository,
                                   const char *domain_id, flowie_control_database_t *database,
                                   flowie_control_data_transfer_result_t *result) {
  flowie_control_membership_view_t items[FLOWIE_CONTROL_PAGE_MAX];
  flowie_control_statement_t *statement = NULL;
  char after_principal[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  char after_group[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  int more = 0;
  int rc = data_prepare(database,
                        "INSERT INTO flowie_control_data_membership(domain_id,principal_id,"
                        "group_id) VALUES(?1,?2,?3)",
                        &statement);
  do {
    size_t count = 0u;
    for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
      items[index] = (flowie_control_membership_view_t)FLOWIE_CONTROL_MEMBERSHIP_VIEW_INIT;
    if (rc == TURBO_OK)
      rc = repository->group->membership_list(
          repository->ctx, domain_id, after_principal[0] ? after_principal : NULL,
          after_group[0] ? after_group : NULL, items, FLOWIE_CONTROL_PAGE_MAX, &count, &more);
    for (size_t index = 0u; rc == TURBO_OK && index < count; ++index) {
      rc = data_insert_relation(statement, items[index].domain_id, items[index].principal_id,
                                items[index].group_id);
      if (rc == TURBO_OK) ++result->membership_count;
    }
    if (rc == TURBO_OK && count != 0u) {
      (void)snprintf(after_principal, sizeof(after_principal), "%s",
                     items[count - 1u].principal_id);
      (void)snprintf(after_group, sizeof(after_group), "%s", items[count - 1u].group_id);
    }
  } while (rc == TURBO_OK && more);
  if (statement) (void)flowie_control_database_finalize(statement);
  return rc;
}

static int data_export_roles(const flowie_control_repository_t *repository, const char *domain_id,
                             flowie_control_database_t *database,
                             flowie_control_data_transfer_result_t *result) {
  flowie_control_role_view_t items[FLOWIE_CONTROL_PAGE_MAX];
  flowie_control_statement_t *statement = NULL;
  char after[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  int more = 0;
  int rc = data_prepare(database,
                        "INSERT INTO flowie_control_data_role(domain_id,role_id,enabled) "
                        "VALUES(?1,?2,?3)",
                        &statement);
  do {
    size_t count = 0u;
    for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
      items[index] = (flowie_control_role_view_t)FLOWIE_CONTROL_ROLE_VIEW_INIT;
    if (rc == TURBO_OK)
      rc = repository->role->list(repository->ctx, domain_id, after[0] ? after : NULL, items,
                                  FLOWIE_CONTROL_PAGE_MAX, &count, &more);
    for (size_t index = 0u; rc == TURBO_OK && index < count; ++index) {
      rc = data_insert_role(statement, &items[index]);
      if (rc == TURBO_OK) ++result->role_count;
    }
    if (rc == TURBO_OK && count != 0u)
      (void)snprintf(after, sizeof(after), "%s", items[count - 1u].role_id);
  } while (rc == TURBO_OK && more);
  if (statement) (void)flowie_control_database_finalize(statement);
  return rc;
}

static int data_export_assignments(const flowie_control_repository_t *repository,
                                   const char *domain_id, flowie_control_database_t *database,
                                   flowie_control_data_transfer_result_t *result) {
  flowie_control_user_role_view_t items[FLOWIE_CONTROL_PAGE_MAX];
  flowie_control_statement_t *statement = NULL;
  char after_principal[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  char after_role[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  int more = 0;
  int rc = data_prepare(database,
                        "INSERT INTO flowie_control_data_user_role(domain_id,principal_id,role_id) "
                        "VALUES(?1,?2,?3)",
                        &statement);
  do {
    size_t count = 0u;
    for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
      items[index] = (flowie_control_user_role_view_t)FLOWIE_CONTROL_USER_ROLE_VIEW_INIT;
    if (rc == TURBO_OK)
      rc = repository->role->assignment_list(
          repository->ctx, domain_id, after_principal[0] ? after_principal : NULL,
          after_role[0] ? after_role : NULL, items, FLOWIE_CONTROL_PAGE_MAX, &count, &more);
    for (size_t index = 0u; rc == TURBO_OK && index < count; ++index) {
      rc = data_insert_relation(statement, items[index].domain_id, items[index].principal_id,
                                items[index].role_id);
      if (rc == TURBO_OK) ++result->assignment_count;
    }
    if (rc == TURBO_OK && count != 0u) {
      (void)snprintf(after_principal, sizeof(after_principal), "%s",
                     items[count - 1u].principal_id);
      (void)snprintf(after_role, sizeof(after_role), "%s", items[count - 1u].role_id);
    }
  } while (rc == TURBO_OK && more);
  if (statement) (void)flowie_control_database_finalize(statement);
  return rc;
}

static int data_export_policy(const flowie_control_repository_t *repository, const char *domain_id,
                              flowie_control_database_t *database,
                              flowie_control_data_transfer_result_t *result) {
  flowie_control_policy_subject_rule_view_t items[FLOWIE_CONTROL_DATA_POLICY_PAGE_MAX];
  flowie_control_statement_t *statement = NULL;
  flowie_control_policy_status_t policy = FLOWIE_CONTROL_POLICY_STATUS_INIT;
  char text[FLOWIE_CONTROL_DATA_RULE_TEXT_MAX + 1u];
  int rc = data_prepare(database,
                        "INSERT INTO flowie_control_data_policy_rule(domain_id,subject_kind,"
                        "subject_id,ordinal,rule_document) VALUES(?1,?2,?3,?4,?5)",
                        &statement);
  for (int kind = FLOWIE_SECURITY_SUBJECT_PRINCIPAL;
       rc == TURBO_OK && kind <= FLOWIE_SECURITY_SUBJECT_GROUP; ++kind) {
    uint32_t after = 0u;
    int has_after = 0;
    int more = 0;
    do {
      size_t count = 0u;
      for (size_t index = 0u; index < FLOWIE_CONTROL_DATA_POLICY_PAGE_MAX; ++index)
        items[index] =
            (flowie_control_policy_subject_rule_view_t)FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT;
      rc = repository->policy->subject_rule_list(
          repository->ctx, domain_id, (flowie_security_subject_kind_t)kind, after, has_after, items,
          FLOWIE_CONTROL_DATA_POLICY_PAGE_MAX, &count, &more);
      for (size_t index = 0u; rc == TURBO_OK && index < count; ++index) {
        size_t text_size = 0u;
        rc = flowie_control_acl_format(&items[index].document, text, sizeof(text), &text_size);
        if (rc == TURBO_OK) rc = data_bind_text(statement, 1, domain_id);
        if (rc == TURBO_OK &&
            flowie_control_database_bind_int(statement, 2, kind) != FLOWIE_CONTROL_DB_OK)
          rc = TURBO_EIO;
        if (rc == TURBO_OK) rc = data_bind_text(statement, 3, items[index].document.subject);
        if (rc == TURBO_OK &&
            flowie_control_database_bind_int64(statement, 4, (int64_t)items[index].ordinal) !=
                FLOWIE_CONTROL_DB_OK)
          rc = TURBO_EIO;
        if (rc == TURBO_OK) rc = data_bind_text(statement, 5, text);
        if (rc == TURBO_OK) rc = data_statement_done(statement);
        if (rc == TURBO_OK) ++result->policy_rule_count;
      }
      if (rc == TURBO_OK && count != 0u) {
        after = items[count - 1u].ordinal;
        has_after = 1;
      }
    } while (rc == TURBO_OK && more);
  }
  if (statement) (void)flowie_control_database_finalize(statement);
  if (rc == TURBO_OK) rc = repository->policy->status(repository->ctx, domain_id, &policy);
  if (rc == TURBO_OK) {
    rc = data_prepare(database,
                      "INSERT INTO flowie_control_data_policy_status(domain_id,published,"
                      "expires_at) VALUES(?1,?2,?3)",
                      &statement);
    if (rc == TURBO_OK) rc = data_bind_text(statement, 1, domain_id);
    result->policy_published = policy.policy_version != 0u;
    if (rc == TURBO_OK && flowie_control_database_bind_int(
                              statement, 2, result->policy_published) != FLOWIE_CONTROL_DB_OK)
      rc = TURBO_EIO;
    if (rc == TURBO_OK && flowie_control_database_bind_int64(
                              statement, 3, (int64_t)policy.expires_at) != FLOWIE_CONTROL_DB_OK)
      rc = TURBO_EIO;
    if (rc == TURBO_OK) rc = data_statement_done(statement);
  }
  if (statement) (void)flowie_control_database_finalize(statement);
  return rc;
}

int flowie_control_data_export(const flowie_control_repository_t *repository, const char *domain_id,
                               const char *output_path,
                               flowie_control_data_transfer_result_t *result) {
  flowie_control_data_transfer_result_t value = FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
  flowie_control_domain_view_t domain = FLOWIE_CONTROL_DOMAIN_VIEW_INIT;
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  char temporary_path[FLOWIE_CONTROL_DATA_PATH_MAX + 1u];
  uint64_t final_revision = 0u;
  int published = 0;
  int rc;
  if (!repository || !domain_id || !output_path || !result || result->size < sizeof(*result) ||
      strcmp(domain_id, FLOWIE_CONTROL_SYSTEM_DOMAIN) == 0 ||
      strlen(output_path) > FLOWIE_CONTROL_DATA_PATH_MAX - 16u)
    return TURBO_EINVAL;
  *result = value;
  if (turbo_fs_access(output_path, TURBO_FS_ACCESS_EXISTS) == TURBO_OK) return TURBO_EALREADY;
  (void)snprintf(temporary_path, sizeof(temporary_path), "%s.flowie.tmp", output_path);
  if (turbo_fs_access(temporary_path, TURBO_FS_ACCESS_EXISTS) == TURBO_OK) return TURBO_EALREADY;
  rc = flowie_control_repository_validate(repository);
  if (rc == TURBO_OK) rc = repository->user->domain_get(repository->ctx, domain_id, &domain);
  if (rc == TURBO_OK) rc = repository->audit->revision(repository->ctx, &value.source_revision);
  if (rc == TURBO_OK) rc = data_db_open(temporary_path, "read_write_create", &database);
  if (rc == TURBO_OK) rc = data_db_exec(database, flowie_control_data_schema_sql(NULL));
  if (rc == TURBO_OK) rc = data_db_exec(database, "BEGIN IMMEDIATE");
  if (rc == TURBO_OK)
    rc = data_prepare(database,
                      "INSERT INTO flowie_control_data_domain(domain_id,source_revision) "
                      "VALUES(?1,?2)",
                      &statement);
  if (rc == TURBO_OK) rc = data_bind_text(statement, 1, domain_id);
  if (rc == TURBO_OK && flowie_control_database_bind_int64(
                            statement, 2, (int64_t)value.source_revision) != FLOWIE_CONTROL_DB_OK)
    rc = TURBO_EIO;
  if (rc == TURBO_OK) rc = data_statement_done(statement);
  if (statement) {
    (void)flowie_control_database_finalize(statement);
    statement = NULL;
  }
  if (rc == TURBO_OK) rc = data_export_users(repository, domain_id, database, &value);
  if (rc == TURBO_OK) rc = data_export_groups(repository, domain_id, database, &value);
  if (rc == TURBO_OK) rc = data_export_memberships(repository, domain_id, database, &value);
  if (rc == TURBO_OK) rc = data_export_roles(repository, domain_id, database, &value);
  if (rc == TURBO_OK) rc = data_export_assignments(repository, domain_id, database, &value);
  if (rc == TURBO_OK) rc = data_export_policy(repository, domain_id, database, &value);
  if (rc == TURBO_OK) rc = repository->audit->revision(repository->ctx, &final_revision);
  if (rc == TURBO_OK && final_revision != value.source_revision) rc = TURBO_EBUSY;
  if (rc == TURBO_OK) rc = data_db_exec(database, "COMMIT");
  if (rc == TURBO_OK) published = 1;
  if (!published && database) (void)data_db_exec(database, "ROLLBACK");
  if (database && flowie_control_database_close(database) != FLOWIE_CONTROL_DB_OK && rc == TURBO_OK)
    rc = TURBO_EIO;
  if (rc == TURBO_OK) rc = turbo_fs_rename(temporary_path, output_path);
  if (rc != TURBO_OK) (void)turbo_fs_unlink(temporary_path);
  if (rc == TURBO_OK) *result = value;
  return rc;
}

static int data_source_preflight(flowie_control_database_t *database, char *domain_id,
                                 size_t domain_capacity, uint64_t *source_revision, int *published,
                                 uint64_t *expires_at, size_t *policy_rule_count) {
  flowie_control_statement_t *statement = NULL;
  flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
  char text[FLOWIE_CONTROL_DATA_RULE_TEXT_MAX + 1u];
  char canonical[FLOWIE_CONTROL_DATA_RULE_TEXT_MAX + 1u];
  int status = FLOWIE_CONTROL_DB_DONE;
  int64_t value = 0;
  int rc = data_db_exec(database, "BEGIN");
  if (rc == TURBO_OK)
    rc = data_prepare(database,
                      "SELECT d.domain_id,d.source_revision,s.published,s.expires_at "
                      "FROM flowie_control_data_domain d JOIN flowie_control_data_policy_status s "
                      "ON s.domain_id=d.domain_id WHERE (SELECT format_version FROM "
                      "flowie_control_data_metadata WHERE singleton=1)=1 AND "
                      "(SELECT COUNT(*) FROM flowie_control_data_domain)=1",
                      &statement);
  if (rc == TURBO_OK && (status = flowie_control_database_step(statement)) != FLOWIE_CONTROL_DB_ROW)
    rc = status == FLOWIE_CONTROL_DB_DONE ? TURBO_EPROTO : data_db_status(status);
  if (rc == TURBO_OK) rc = data_column_text(statement, 0, domain_id, domain_capacity, 0);
  if (rc == TURBO_OK &&
      (flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_INTEGER ||
       (value = flowie_control_database_column_int64(statement, 1)) <= 0))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK) *source_revision = (uint64_t)value;
  if (rc == TURBO_OK &&
      (flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_INTEGER ||
       ((*published = flowie_control_database_column_int(statement, 2)) != 0 && *published != 1)))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK &&
      (flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
       (value = flowie_control_database_column_int64(statement, 3)) < 0))
    rc = TURBO_EPROTO;
  if (rc == TURBO_OK) *expires_at = (uint64_t)value;
  if (rc == TURBO_OK && flowie_control_database_step(statement) != FLOWIE_CONTROL_DB_DONE)
    rc = TURBO_EPROTO;
  if (statement) {
    (void)flowie_control_database_finalize(statement);
    statement = NULL;
  }
  if (rc == TURBO_OK && strcmp(domain_id, FLOWIE_CONTROL_SYSTEM_DOMAIN) == 0) rc = TURBO_EINVAL;
  if (rc == TURBO_OK) rc = data_source_shape_preflight(database);
  if (rc == TURBO_OK)
    rc = data_prepare(database,
                      "SELECT subject_kind,subject_id,ordinal,rule_document "
                      "FROM flowie_control_data_policy_rule ORDER BY subject_kind,subject_id",
                      &statement);
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    char subject_id[FLOWIE_SECURITY_ID_MAX + 1u];
    size_t text_size;
    size_t canonical_size = 0u;
    const int subject_kind = flowie_control_database_column_int(statement, 0);
    const int64_t ordinal = flowie_control_database_column_int64(statement, 2);
    if (flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        subject_kind < FLOWIE_SECURITY_SUBJECT_PRINCIPAL ||
        subject_kind > FLOWIE_SECURITY_SUBJECT_GROUP ||
        flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_INTEGER ||
        ordinal < 0 || ordinal > 4095)
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) rc = data_column_text(statement, 1, subject_id, sizeof(subject_id), 0);
    if (rc == TURBO_OK) rc = data_column_text(statement, 3, text, sizeof(text), 0);
    text_size = rc == TURBO_OK ? strlen(text) : 0u;
    if (rc == TURBO_OK) rc = flowie_control_acl_parse(text, text_size, &document);
    if (rc == TURBO_OK)
      rc = flowie_control_acl_format(&document, canonical, sizeof(canonical), &canonical_size);
    if (rc == TURBO_OK && (canonical_size != text_size || memcmp(canonical, text, text_size) != 0))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK && (document.subject_kind != (flowie_security_subject_kind_t)subject_kind ||
                           strcmp(document.subject, subject_id) != 0))
      rc = TURBO_EPROTO;
    if (rc == TURBO_OK) ++*policy_rule_count;
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);
  if (statement) {
    (void)flowie_control_database_finalize(statement);
    statement = NULL;
  }
  if (rc == TURBO_OK)
    rc = data_prepare(database, "SELECT * FROM pragma_foreign_key_check LIMIT 1", &statement);
  if (rc == TURBO_OK &&
      (status = flowie_control_database_step(statement)) != FLOWIE_CONTROL_DB_DONE)
    rc = status == FLOWIE_CONTROL_DB_ROW ? TURBO_EPROTO : data_db_status(status);
  if (statement) (void)flowie_control_database_finalize(statement);
  return rc;
}

static int data_import_revision(flowie_control_management_service_t *service,
                                const flowie_control_management_caller_t *caller,
                                uint64_t *revision) {
  return flowie_control_management_current_revision(service, caller, revision);
}

static int data_target_preflight(const flowie_control_repository_t *repository,
                                 const char *import_domain_id) {
  flowie_control_domain_view_t items[FLOWIE_CONTROL_PAGE_MAX];
  char after[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  int more = 0;
  int rc = TURBO_OK;
  do {
    size_t count = 0u;
    for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
      items[index] = (flowie_control_domain_view_t)FLOWIE_CONTROL_DOMAIN_VIEW_INIT;
    rc = repository->user->domain_list(repository->ctx, after[0] ? after : NULL, items,
                                       FLOWIE_CONTROL_PAGE_MAX, &count, &more);
    for (size_t index = 0u; rc == TURBO_OK && index < count; ++index) {
      if (strcmp(items[index].domain_id, FLOWIE_CONTROL_SYSTEM_DOMAIN) != 0 &&
          strcmp(items[index].domain_id, import_domain_id) != 0)
        rc = TURBO_EBUSY;
    }
    if (rc == TURBO_OK && count != 0u)
      (void)snprintf(after, sizeof(after), "%s", items[count - 1u].domain_id);
  } while (rc == TURBO_OK && more);
  return rc;
}

static uint64_t data_import_namespace(const char *domain_id, uint64_t source_revision) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (const unsigned char *cursor = (const unsigned char *)domain_id; *cursor; ++cursor) {
    hash ^= *cursor;
    hash *= UINT64_C(1099511628211);
  }
  for (size_t shift = 0u; shift < 64u; shift += 8u) {
    hash ^= (source_revision >> shift) & UINT64_C(0xff);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static int data_request_id(char *output, size_t capacity, const char *domain_id,
                           uint64_t source_revision, const char *kind, size_t sequence) {
  const int count = snprintf(output, capacity, "flowie-data-v1-%016" PRIx64 "-%s-%08zu",
                             data_import_namespace(domain_id, source_revision), kind, sequence);
  return count > 0 && (size_t)count < capacity ? TURBO_OK : TURBO_ENOSPC;
}

static int data_import_rows(flowie_control_database_t *database,
                            flowie_control_management_service_t *service,
                            const flowie_control_management_caller_t *caller, const char *domain_id,
                            uint64_t source_revision,
                            flowie_control_data_transfer_result_t *result) {
  flowie_control_management_caller_t domain_caller = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  flowie_control_statement_t *statement = NULL;
  flowie_control_command_result_t command_result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  char request_id[FLOWIE_CONTROL_REQUEST_ID_MAX + 1u];
  char first[FLOWIE_SECURITY_ID_MAX + 1u];
  char second[FLOWIE_SECURITY_ID_MAX + 1u];
  char third[FLOWIE_SECURITY_TYPE_MAX + 1u];
  char text[FLOWIE_CONTROL_DATA_RULE_TEXT_MAX + 1u];
  uint64_t revision = 0u;
  size_t sequence = 0u;
  int status = FLOWIE_CONTROL_DB_DONE;
  int rc;
  flowie_control_domain_create_command_t domain = FLOWIE_CONTROL_DOMAIN_CREATE_COMMAND_INIT;
  rc = data_import_revision(service, caller, &revision);
  if (rc == TURBO_OK)
    rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision, "domain",
                         sequence++);
  domain.domain_id = domain_id;
  domain.actor = caller->actor;
  domain.request_id = request_id;
  domain.expected_revision = revision;
  domain.occurred_at = source_revision + sequence;
  if (rc == TURBO_OK)
    rc = flowie_control_management_domain_create(service, caller, &domain, &command_result);
  if (rc == TURBO_OK) {
    domain_caller = *caller;
    domain_caller.domain_id = domain_id;
    caller = &domain_caller;
  }

#define DATA_IMPORT_PREPARE(query)                                                                 \
  do {                                                                                             \
    if (statement) {                                                                               \
      (void)flowie_control_database_finalize(statement);                                           \
      statement = NULL;                                                                            \
    }                                                                                              \
    if (rc == TURBO_OK) rc = data_prepare(database, query, &statement);                            \
  } while (0)

  DATA_IMPORT_PREPARE("SELECT principal_id,principal_type FROM flowie_control_data_user "
                      "ORDER BY principal_id");
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_user_create_command_t create = FLOWIE_CONTROL_USER_CREATE_COMMAND_INIT;
    rc = data_column_text(statement, 0, first, sizeof(first), 0);
    if (rc == TURBO_OK) rc = data_column_text(statement, 1, third, sizeof(third), 0);
    if (rc == TURBO_OK) rc = data_import_revision(service, caller, &revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision, "user",
                           sequence++);
    create.domain_id = domain_id;
    create.principal_id = first;
    create.principal_type = third;
    create.actor = caller->actor;
    create.request_id = request_id;
    create.expected_revision = revision;
    create.occurred_at = source_revision + sequence;
    if (rc == TURBO_OK)
      rc = flowie_control_management_user_create(service, caller, &create, &command_result);
    if (rc == TURBO_OK) ++result->user_count;
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);

  DATA_IMPORT_PREPARE("SELECT group_id,parent_group_id FROM flowie_control_data_group "
                      "ORDER BY depth,group_id");
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_group_create_command_t create = FLOWIE_CONTROL_GROUP_CREATE_COMMAND_INIT;
    rc = data_column_text(statement, 0, first, sizeof(first), 0);
    if (rc == TURBO_OK) rc = data_column_text(statement, 1, second, sizeof(second), 1);
    if (rc == TURBO_OK) rc = data_import_revision(service, caller, &revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision, "group",
                           sequence++);
    create.domain_id = domain_id;
    create.group_id = first;
    create.parent_group_id = second[0] ? second : NULL;
    create.actor = caller->actor;
    create.request_id = request_id;
    create.expected_revision = revision;
    create.occurred_at = source_revision + sequence;
    if (rc == TURBO_OK)
      rc = flowie_control_management_group_create(service, caller, &create, &command_result);
    if (rc == TURBO_OK) ++result->group_count;
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);

  DATA_IMPORT_PREPARE("SELECT role_id FROM flowie_control_data_role ORDER BY role_id");
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_role_create_command_t create = FLOWIE_CONTROL_ROLE_CREATE_COMMAND_INIT;
    rc = data_column_text(statement, 0, first, sizeof(first), 0);
    if (rc == TURBO_OK) rc = data_import_revision(service, caller, &revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision, "role",
                           sequence++);
    create.domain_id = domain_id;
    create.role_id = first;
    create.actor = caller->actor;
    create.request_id = request_id;
    create.expected_revision = revision;
    create.occurred_at = source_revision + sequence;
    if (rc == TURBO_OK)
      rc = flowie_control_management_role_create(service, caller, &create, &command_result);
    if (rc == TURBO_OK) ++result->role_count;
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);

  DATA_IMPORT_PREPARE("SELECT principal_id,group_id FROM flowie_control_data_membership "
                      "ORDER BY principal_id,group_id");
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_membership_add_command_t add = FLOWIE_CONTROL_MEMBERSHIP_ADD_COMMAND_INIT;
    rc = data_column_text(statement, 0, first, sizeof(first), 0);
    if (rc == TURBO_OK) rc = data_column_text(statement, 1, second, sizeof(second), 0);
    if (rc == TURBO_OK) rc = data_import_revision(service, caller, &revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision, "membership",
                           sequence++);
    add.domain_id = domain_id;
    add.principal_id = first;
    add.group_id = second;
    add.actor = caller->actor;
    add.request_id = request_id;
    add.expected_revision = revision;
    add.occurred_at = source_revision + sequence;
    if (rc == TURBO_OK)
      rc = flowie_control_management_membership_add(service, caller, &add, &command_result);
    if (rc == TURBO_OK) ++result->membership_count;
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);

  DATA_IMPORT_PREPARE("SELECT principal_id,role_id FROM flowie_control_data_user_role "
                      "ORDER BY principal_id,role_id");
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_user_role_add_command_t add = FLOWIE_CONTROL_USER_ROLE_ADD_COMMAND_INIT;
    rc = data_column_text(statement, 0, first, sizeof(first), 0);
    if (rc == TURBO_OK) rc = data_column_text(statement, 1, second, sizeof(second), 0);
    if (rc == TURBO_OK) rc = data_import_revision(service, caller, &revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision, "assignment",
                           sequence++);
    add.domain_id = domain_id;
    add.principal_id = first;
    add.role_id = second;
    add.actor = caller->actor;
    add.request_id = request_id;
    add.expected_revision = revision;
    add.occurred_at = source_revision + sequence;
    if (rc == TURBO_OK)
      rc = flowie_control_management_user_role_add(service, caller, &add, &command_result);
    if (rc == TURBO_OK) ++result->assignment_count;
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);

  DATA_IMPORT_PREPARE("SELECT ordinal,rule_document FROM flowie_control_data_policy_rule "
                      "ORDER BY ordinal");
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_policy_subject_rule_put_command_t put =
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_PUT_COMMAND_INIT;
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    const int64_t ordinal = flowie_control_database_column_int64(statement, 0);
    rc = data_column_text(statement, 1, text, sizeof(text), 0);
    if (rc == TURBO_OK) rc = flowie_control_acl_parse(text, strlen(text), &document);
    if (rc == TURBO_OK) rc = data_import_revision(service, caller, &revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision, "policy",
                           sequence++);
    put.domain_id = domain_id;
    put.ordinal = (uint32_t)ordinal;
    put.document = &document;
    put.actor = caller->actor;
    put.request_id = request_id;
    put.expected_revision = revision;
    put.occurred_at = source_revision + sequence;
    if (rc == TURBO_OK)
      rc =
          flowie_control_management_policy_subject_rule_put(service, caller, &put, &command_result);
    if (rc == TURBO_OK) ++result->policy_rule_count;
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);
  if (statement) {
    (void)flowie_control_database_finalize(statement);
    statement = NULL;
  }
#undef DATA_IMPORT_PREPARE
  return rc;
}

static int data_import_disabled_rows(flowie_control_database_t *database,
                                     flowie_control_management_service_t *service,
                                     const flowie_control_management_caller_t *caller,
                                     const char *domain_id, uint64_t source_revision) {
  flowie_control_statement_t *statement = NULL;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  char id[FLOWIE_SECURITY_ID_MAX + 1u];
  char request_id[FLOWIE_CONTROL_REQUEST_ID_MAX + 1u];
  uint64_t revision = 0u;
  size_t sequence = 0u;
  int status = FLOWIE_CONTROL_DB_DONE;
  int rc = data_prepare(database,
                        "SELECT principal_id FROM flowie_control_data_user WHERE enabled=0 "
                        "ORDER BY principal_id",
                        &statement);
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_user_disable_command_t disable = FLOWIE_CONTROL_USER_DISABLE_COMMAND_INIT;
    rc = data_column_text(statement, 0, id, sizeof(id), 0);
    if (rc == TURBO_OK) rc = data_import_revision(service, caller, &revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision,
                           "user-disable", sequence++);
    disable.domain_id = domain_id;
    disable.principal_id = id;
    disable.actor = caller->actor;
    disable.request_id = request_id;
    disable.expected_revision = revision;
    disable.occurred_at = source_revision + sequence;
    if (rc == TURBO_OK)
      rc = flowie_control_management_user_disable(service, caller, &disable, &result);
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);
  if (statement) {
    (void)flowie_control_database_finalize(statement);
    statement = NULL;
  }
  if (rc == TURBO_OK)
    rc = data_prepare(database,
                      "SELECT role_id FROM flowie_control_data_role WHERE enabled=0 "
                      "ORDER BY role_id",
                      &statement);
  status = FLOWIE_CONTROL_DB_DONE;
  while (rc == TURBO_OK &&
         (status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_role_disable_command_t disable = FLOWIE_CONTROL_ROLE_DISABLE_COMMAND_INIT;
    rc = data_column_text(statement, 0, id, sizeof(id), 0);
    if (rc == TURBO_OK) rc = data_import_revision(service, caller, &revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, source_revision,
                           "role-disable", sequence++);
    disable.domain_id = domain_id;
    disable.role_id = id;
    disable.actor = caller->actor;
    disable.request_id = request_id;
    disable.expected_revision = revision;
    disable.occurred_at = source_revision + sequence;
    if (rc == TURBO_OK)
      rc = flowie_control_management_role_disable(service, caller, &disable, &result);
  }
  if (rc == TURBO_OK && status != FLOWIE_CONTROL_DB_DONE) rc = data_db_status(status);
  if (statement) (void)flowie_control_database_finalize(statement);
  return rc;
}

int flowie_control_data_import(const flowie_control_repository_t *repository,
                               const char *input_path, int dry_run,
                               flowie_control_data_transfer_result_t *result) {
  flowie_control_data_transfer_result_t value = FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
  flowie_control_management_service_config_t service_config =
      FLOWIE_CONTROL_MANAGEMENT_SERVICE_CONFIG_INIT;
  flowie_control_management_caller_t caller = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  flowie_control_management_service_t *service = NULL;
  flowie_control_database_t *database = NULL;
  char domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  uint64_t initial_target_revision = 0u;
  uint64_t expires_at = 0u;
  size_t validated_policy_count = 0u;
  int target_revision_known = 0;
  int rc;
  if (!repository || !input_path || !input_path[0] || !result || result->size < sizeof(*result) ||
      (dry_run != 0 && dry_run != 1))
    return TURBO_EINVAL;
  *result = value;
  rc = flowie_control_repository_validate(repository);
  if (rc == TURBO_OK) rc = data_db_open(input_path, "read_only", &database);
  if (rc == TURBO_OK)
    rc = data_source_preflight(database, domain_id, sizeof(domain_id), &value.source_revision,
                               &value.policy_published, &expires_at, &validated_policy_count);
  if (rc == TURBO_OK) rc = data_target_preflight(repository, domain_id);
  service_config.repository = repository;
  if (rc == TURBO_OK) rc = flowie_control_management_service_create(&service_config, &service);
  caller.domain_id = FLOWIE_CONTROL_SYSTEM_DOMAIN;
  caller.actor = "flowie-control-data";
  caller.permissions = FLOWIE_CONTROL_MANAGEMENT_SYSTEM_ADMIN |
                       FLOWIE_CONTROL_MANAGEMENT_USER_ADMIN |
                       FLOWIE_CONTROL_MANAGEMENT_POLICY_ADMIN |
                       FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN | FLOWIE_CONTROL_MANAGEMENT_VIEWER;
  if (rc == TURBO_OK) rc = data_import_revision(service, &caller, &initial_target_revision);
  if (rc == TURBO_OK) target_revision_known = 1;
  value.target_revision = initial_target_revision;
  if (rc == TURBO_OK && !dry_run)
    rc = data_import_rows(database, service, &caller, domain_id, value.source_revision, &value);
  if (rc == TURBO_OK && !dry_run) caller.domain_id = domain_id;
  if (rc == TURBO_OK && !dry_run && value.policy_published) {
    flowie_control_policy_publish_command_t publish = FLOWIE_CONTROL_POLICY_PUBLISH_COMMAND_INIT;
    flowie_control_policy_publish_result_t published = FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
    char request_id[FLOWIE_CONTROL_REQUEST_ID_MAX + 1u];
    rc = data_import_revision(service, &caller, &value.target_revision);
    if (rc == TURBO_OK)
      rc = data_request_id(request_id, sizeof(request_id), domain_id, value.source_revision,
                           "publish", validated_policy_count);
    publish.domain_id = domain_id;
    publish.actor = caller.actor;
    publish.request_id = request_id;
    publish.expected_revision = value.target_revision;
    publish.occurred_at = value.source_revision + validated_policy_count + 1u;
    publish.expires_at = expires_at;
    if (rc == TURBO_OK)
      rc = flowie_control_management_policy_publish(service, &caller, &publish, &published);
  }
  if (rc == TURBO_OK && !dry_run)
    rc = data_import_disabled_rows(database, service, &caller, domain_id, value.source_revision);
  if (rc == TURBO_OK) rc = data_import_revision(service, &caller, &value.target_revision);
  if (rc == TURBO_OK) {
    value.mutated = value.target_revision != initial_target_revision;
    *result = value;
  } else if (target_revision_known) {
    uint64_t failed_revision = initial_target_revision;
    if (repository->audit->revision(repository->ctx, &failed_revision) == TURBO_OK) {
      value.target_revision = failed_revision;
      value.mutated = failed_revision != initial_target_revision;
      *result = value;
    }
  }
  if (service) flowie_control_management_service_destroy(service);
  if (database) {
    (void)data_db_exec(database, "ROLLBACK");
    (void)flowie_control_database_close(database);
  }
  return rc;
}
