#include "flowie_control_credential_internal.h"
#include "flowie_control_acl_internal.h"
#include "flowie_control_store_internal.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "flowie_security_sqlite.h"
#include "turbo_thread.h"

#include <sqlite3.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int control_domain_create(flowie_control_store_t *store, const char *domain_id,
                               const char *request_id, uint64_t expected_revision,
                               flowie_control_command_result_t *result) {
  flowie_control_domain_create_command_t command =
      FLOWIE_CONTROL_DOMAIN_CREATE_COMMAND_INIT;
  command.domain_id = domain_id;
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 1000u + expected_revision;
  return flowie_control_store_domain_create(store, &command, result);
}

static flowie_control_store_t *control_store_open(char **path_out) {
  flowie_control_store_config_t config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  flowie_control_store_t *store = NULL;
  *path_out = tt_make_temp_file("flowie-control", ".sqlite3");
  check_not_null(*path_out);
  config.database_path = *path_out;
  check_equal(flowie_control_store_open(&config, &store), TURBO_OK);
  check_not_null(store);
  check_equal(control_domain_create(store, "root-a", "request-root-a", 0u, &result), TURBO_OK);
  check_equal(result.revision, 1u);
  return store;
}

static void control_store_close(flowie_control_store_t *store, char *path) {
  flowie_control_store_destroy(store);
  check_equal(tt_remove_file(path), 0);
  free(path);
}

static int control_store_mark_group_disabled(const char *path, const char *domain_id,
                                             const char *group_id) {
  static const char sql[] =
      "UPDATE flowie_control_group SET enabled=0 WHERE domain_id=?1 AND group_id=?2";
  sqlite3 *database = NULL;
  sqlite3_stmt *statement = NULL;
  int status;
  int rc = -1;
  if (!path || !domain_id || !group_id || sqlite3_open_v2(path, &database, SQLITE_OPEN_READWRITE,
                                                          NULL) != SQLITE_OK)
    goto done;
  if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK ||
      sqlite3_bind_text(statement, 1, domain_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(statement, 2, group_id, -1, SQLITE_TRANSIENT) != SQLITE_OK)
    goto done;
  status = sqlite3_step(statement);
  if (status == SQLITE_DONE && sqlite3_changes(database) == 1) rc = 0;

done:
  if (statement) (void)sqlite3_finalize(statement);
  if (database) (void)sqlite3_close(database);
  return rc;
}

static flowie_control_user_create_command_t
control_user_create_command(const char *request_id, uint64_t expected_revision) {
  flowie_control_user_create_command_t command = FLOWIE_CONTROL_USER_CREATE_COMMAND_INIT;
  command.domain_id = "root-a";
  command.principal_id = "device-7";
  command.principal_type = "device";
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 2000u + expected_revision;
  return command;
}

static flowie_control_credential_issue_command_t
control_credential_issue_command(const char *request_id, uint64_t expected_revision) {
  flowie_control_credential_issue_command_t command = FLOWIE_CONTROL_CREDENTIAL_ISSUE_COMMAND_INIT;
  command.domain_id = "root-a";
  command.principal_id = "device-7";
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 2500u + expected_revision;
  return command;
}

static int control_credential_revoke(flowie_control_store_t *store, const char *request_id,
                                     uint64_t expected_revision,
                                     flowie_control_command_result_t *result) {
  flowie_control_credential_revoke_command_t command =
      FLOWIE_CONTROL_CREDENTIAL_REVOKE_COMMAND_INIT;
  command.domain_id = "root-a";
  command.principal_id = "device-7";
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 2750u + expected_revision;
  return flowie_control_store_credential_revoke(store, &command, result);
}

static int control_group_create(flowie_control_store_t *store, const char *domain_id,
                                const char *group_id, const char *parent_group_id,
                                const char *request_id, uint64_t expected_revision,
                                flowie_control_command_result_t *result) {
  flowie_control_group_create_command_t command = FLOWIE_CONTROL_GROUP_CREATE_COMMAND_INIT;
  command.domain_id = domain_id;
  command.group_id = group_id;
  command.parent_group_id = parent_group_id;
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 3000u + expected_revision;
  return flowie_control_store_group_create(store, &command, result);
}

static int control_membership_add(flowie_control_store_t *store, const char *group_id,
                                  const char *request_id, uint64_t expected_revision,
                                  flowie_control_command_result_t *result) {
  flowie_control_membership_add_command_t command = FLOWIE_CONTROL_MEMBERSHIP_ADD_COMMAND_INIT;
  command.domain_id = "root-a";
  command.principal_id = "device-7";
  command.group_id = group_id;
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 4000u + expected_revision;
  return flowie_control_store_membership_add(store, &command, result);
}

static int control_membership_remove(flowie_control_store_t *store, const char *group_id,
                                     const char *request_id, uint64_t expected_revision,
                                     flowie_control_command_result_t *result) {
  flowie_control_membership_remove_command_t command =
      FLOWIE_CONTROL_MEMBERSHIP_REMOVE_COMMAND_INIT;
  command.domain_id = "root-a";
  command.principal_id = "device-7";
  command.group_id = group_id;
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 4500u + expected_revision;
  return flowie_control_store_membership_remove(store, &command, result);
}

static int control_group_delete(flowie_control_store_t *store, const char *domain_id,
                                const char *group_id, const char *request_id,
                                uint64_t expected_revision,
                                flowie_control_command_result_t *result) {
  flowie_control_group_delete_command_t command = FLOWIE_CONTROL_GROUP_DELETE_COMMAND_INIT;
  command.domain_id = domain_id;
  command.group_id = group_id;
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 4750u + expected_revision;
  return flowie_control_store_group_delete(store, &command, result);
}

static int control_role_create(flowie_control_store_t *store, const char *domain_id,
                               const char *role_id, const char *request_id,
                               uint64_t expected_revision,
                               flowie_control_command_result_t *result) {
  flowie_control_role_create_command_t command = FLOWIE_CONTROL_ROLE_CREATE_COMMAND_INIT;
  command.domain_id = domain_id;
  command.role_id = role_id;
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 5000u + expected_revision;
  return flowie_control_store_role_create(store, &command, result);
}

static int control_user_role_add(flowie_control_store_t *store, const char *role_id,
                                 const char *request_id, uint64_t expected_revision,
                                 flowie_control_command_result_t *result) {
  flowie_control_user_role_add_command_t command = FLOWIE_CONTROL_USER_ROLE_ADD_COMMAND_INIT;
  command.domain_id = "root-a";
  command.principal_id = "device-7";
  command.role_id = role_id;
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 6000u + expected_revision;
  return flowie_control_store_user_role_add(store, &command, result);
}

static int control_user_role_remove(flowie_control_store_t *store, const char *role_id,
                                    const char *request_id, uint64_t expected_revision,
                                    flowie_control_command_result_t *result) {
  flowie_control_user_role_remove_command_t command = FLOWIE_CONTROL_USER_ROLE_REMOVE_COMMAND_INIT;
  command.domain_id = "root-a";
  command.principal_id = "device-7";
  command.role_id = role_id;
  command.actor = "admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 7000u + expected_revision;
  return flowie_control_store_user_role_remove(store, &command, result);
}

static int control_policy_rule_put(flowie_control_store_t *store, uint32_t ordinal,
                                   const char *rule_line, const char *request_id,
                                   uint64_t expected_revision,
                                   flowie_control_command_result_t *result) {
  flowie_control_policy_rule_put_command_t command = FLOWIE_CONTROL_POLICY_RULE_PUT_COMMAND_INIT;
  command.domain_id = "root-a";
  command.ordinal = ordinal;
  command.rule_line = rule_line;
  command.actor = "policy-admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 8000u + expected_revision;
  return flowie_control_store_policy_rule_put(store, &command, result);
}

static int control_policy_rule_delete(flowie_control_store_t *store, uint32_t ordinal,
                                      const char *request_id, uint64_t expected_revision,
                                      flowie_control_command_result_t *result) {
  flowie_control_policy_rule_delete_command_t command =
      FLOWIE_CONTROL_POLICY_RULE_DELETE_COMMAND_INIT;
  command.domain_id = "root-a";
  command.ordinal = ordinal;
  command.actor = "policy-admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 8500u + expected_revision;
  return flowie_control_store_policy_rule_delete(store, &command, result);
}

static int control_policy_publish(flowie_control_store_t *store, const char *request_id,
                                  uint64_t expected_revision, uint64_t expires_at,
                                  flowie_control_policy_publish_result_t *result) {
  flowie_control_policy_publish_command_t command = FLOWIE_CONTROL_POLICY_PUBLISH_COMMAND_INIT;
  command.domain_id = "root-a";
  command.actor = "policy-admin-1";
  command.request_id = request_id;
  command.expected_revision = expected_revision;
  command.occurred_at = 9000u + expected_revision;
  command.expires_at = expires_at;
  return flowie_control_store_policy_publish(store, &command, result);
}

typedef struct control_concurrent_group_create_s {
  flowie_control_store_t *store;
  char group_id[64];
  char request_id[64];
  uint64_t expected_revision;
  atomic_int *ready;
  atomic_int *go;
  flowie_control_command_result_t result;
  int rc;
} control_concurrent_group_create_t;

static void control_concurrent_group_create(void *arg) {
  control_concurrent_group_create_t *write = (control_concurrent_group_create_t *)arg;
  atomic_fetch_add_explicit(write->ready, 1, memory_order_release);
  while (!atomic_load_explicit(write->go, memory_order_acquire))
    turbo_thread_yield();
  write->rc = control_group_create(write->store, "root-a", write->group_id, NULL,
                                   write->request_id, write->expected_revision, &write->result);
}

spec("Flowie control SQLite fact store") {
  it("purges every v3 draft and published policy while preserving domain data") {
    static const char v3_sql[] =
        "CREATE TABLE flowie_control_schema_version(singleton INTEGER PRIMARY KEY,version "
        "INTEGER NOT NULL,fingerprint TEXT NOT NULL);"
        "INSERT INTO flowie_control_schema_version VALUES(1,3,"
        "'flowie-control-acl-document-schema-v3-20260805');"
        "CREATE TABLE flowie_control_domain(domain_id TEXT PRIMARY KEY) WITHOUT ROWID;"
        "INSERT INTO flowie_control_domain VALUES('root-a');"
        "CREATE TABLE flowie_control_policy_draft(domain_id TEXT NOT NULL,ordinal INTEGER NOT "
        "NULL,rule_line TEXT NOT NULL,revision INTEGER NOT NULL,updated_at INTEGER NOT NULL,"
        "PRIMARY KEY(domain_id,ordinal)) WITHOUT ROWID;"
        "INSERT INTO flowie_control_policy_draft VALUES('root-a',1,'user old-device allow',1,1);"
        "CREATE TABLE turbo_flow_acl_bundle_v3(namespace_name TEXT PRIMARY KEY,policy_version "
        "INTEGER NOT NULL,expires_at INTEGER NOT NULL);"
        "INSERT INTO turbo_flow_acl_bundle_v3 VALUES('root-a',7,0);"
        "CREATE TABLE turbo_flow_acl_rule_v3(namespace_name TEXT NOT NULL,ordinal INTEGER NOT "
        "NULL,rule_line TEXT NOT NULL,PRIMARY KEY(namespace_name,ordinal)) WITHOUT ROWID;"
        "INSERT INTO turbo_flow_acl_rule_v3 VALUES('root-a',0,'allow principal old-device "
        "root-a connect generic prefix secure-');"
        "CREATE TABLE flowie_control_policy_publish_result(request_id TEXT PRIMARY KEY,"
        "policy_version INTEGER NOT NULL) WITHOUT ROWID;"
        "INSERT INTO flowie_control_policy_publish_result VALUES('old-publish',7);";
    char *path = tt_make_temp_file("flowie-control-v3", ".sqlite3");
    flowie_control_store_config_t config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
    flowie_control_store_t *store = NULL;
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;

    check_not_null(path);
    check_equal(sqlite3_open_v2(path, &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL),
                SQLITE_OK);
    check_equal(sqlite3_exec(database, v3_sql, NULL, NULL, NULL), SQLITE_OK);
    check_equal(sqlite3_close(database), SQLITE_OK);
    database = NULL;

    config.database_path = path;
    check_equal(flowie_control_store_open(&config, &store), TURBO_OK);
    flowie_control_store_destroy(store);
    check_equal(sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(
                    database,
                    "SELECT (SELECT version FROM flowie_control_schema_version WHERE singleton=1),"
                    "(SELECT COUNT(*) FROM flowie_control_domain WHERE domain_id='root-a'),"
                    "(SELECT COUNT(*) FROM flowie_control_policy_draft),"
                    "(SELECT COUNT(*) FROM turbo_flow_acl_bundle_v3),"
                    "(SELECT COUNT(*) FROM turbo_flow_acl_rule_v3),"
                    "(SELECT COUNT(*) FROM flowie_control_policy_publish_result)",
                    -1, &statement, NULL),
                SQLITE_OK);
    check_equal(sqlite3_step(statement), SQLITE_ROW);
    check_equal(sqlite3_column_int(statement, 0), 4);
    check_equal(sqlite3_column_int(statement, 1), 1);
    check_equal(sqlite3_column_int(statement, 2), 0);
    check_equal(sqlite3_column_int(statement, 3), 0);
    check_equal(sqlite3_column_int(statement, 4), 0);
    check_equal(sqlite3_column_int(statement, 5), 0);
    check_equal(sqlite3_finalize(statement), SQLITE_OK);
    check_equal(sqlite3_close(database), SQLITE_OK);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("stores and queries a draft directly by typed subject key") {
    static const char rule_text[] =
        "role publisher allow {\n"
        "  write topic root-a/telemetry/%u/event\n"
        "}";
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    flowie_control_acl_document_t replacement = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    flowie_control_policy_subject_rule_put_command_t put =
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_PUT_COMMAND_INIT;
    flowie_control_policy_subject_rule_view_t view =
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT;
    flowie_control_policy_subject_rule_view_t page[2] = {
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT,
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT};
    flowie_control_policy_subject_rule_delete_command_t remove =
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_DELETE_COMMAND_INIT;
    size_t count = 0u;
    int has_more = 0;

    check_equal(control_role_create(store, "root-a", "publisher", "request-role", 1u,
                                    &result),
                TURBO_OK);
    check_equal(flowie_control_acl_parse(rule_text, sizeof(rule_text) - 1u, &document), TURBO_OK);
    put.domain_id = "root-a";
    put.ordinal = 10u;
    put.document = &document;
    put.actor = "policy-admin-1";
    put.request_id = "request-subject-rule";
    put.expected_revision = 2u;
    put.occurred_at = 8002u;
    check_equal(flowie_control_store_policy_subject_rule_put(store, &put, &result), TURBO_OK);
    check_equal(flowie_control_store_policy_subject_rule_get(
                    store, "root-a", FLOWIE_SECURITY_SUBJECT_ROLE, "publisher", &view),
                TURBO_OK);
    check_equal(view.ordinal, 10u);
    check_equal(view.document.subject_kind, FLOWIE_SECURITY_SUBJECT_ROLE);
    check_equal(view.document.subject, "publisher");
    check_equal(view.document.entry_count, 1u);
    check_equal(view.document.entries[0].topic, "root-a/telemetry/%u/event");

    put.ordinal = 11u;
    put.request_id = "request-subject-rule-replace";
    put.expected_revision = 3u;
    put.occurred_at = 8003u;
    replacement = document;
    replacement.entries[0].effect = FLOWIE_SECURITY_DENY;
    put.document = &replacement;
    check_equal(flowie_control_store_policy_subject_rule_put(store, &put, &result), TURBO_OK);
    check_equal(flowie_control_store_policy_subject_rule_list(
                    store, "root-a", FLOWIE_SECURITY_SUBJECT_ROLE, 0u, 0, page, 2u, &count,
                    &has_more),
                TURBO_OK);
    check_equal(count, 1u);
    check_false(has_more);
    check_equal(page[0].ordinal, 11u);
    check_equal(page[0].document.entries[0].effect, FLOWIE_SECURITY_DENY);

    remove.domain_id = "root-a";
    remove.subject_kind = FLOWIE_SECURITY_SUBJECT_ROLE;
    remove.subject_id = "publisher";
    remove.actor = "policy-admin-1";
    remove.request_id = "request-subject-rule-delete";
    remove.expected_revision = 4u;
    remove.occurred_at = 8004u;
    check_equal(flowie_control_store_policy_subject_rule_delete(store, &remove, &result), TURBO_OK);
    view = (flowie_control_policy_subject_rule_view_t)FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT;
    check_equal(flowie_control_store_policy_subject_rule_get(
                    store, "root-a", FLOWIE_SECURITY_SUBJECT_ROLE, "publisher", &view),
                TURBO_ENOENT);

    control_store_close(store, path);
  }

  it("creates and reads one root-scoped user with an atomic audit revision") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t command =
        control_user_create_command("request-create-1", 1u);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_user_view_t user = FLOWIE_CONTROL_USER_VIEW_INIT;
    uint64_t revision = 0u;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &command, &result), TURBO_OK);
    check_equal(result.revision, 2u);
    check_false(result.replayed);
    check_equal(flowie_control_store_user_get(store, "root-a", "device-7", &user), TURBO_OK);
    check_equal(user.domain_id, "root-a");
    check_equal(user.principal_id, "device-7");
    check_equal(user.principal_type, "device");
    check_true(user.enabled);
    check_equal(user.revision, 2u);
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 2u);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 2u);

    control_store_close(store, path);
  }

  it("replays the same request without duplicating state or audit") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t command =
        control_user_create_command("request-replay", 1u);
    flowie_control_command_result_t first = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_command_result_t replay = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &command, &first), TURBO_OK);
    command.expected_revision = 99u;
    command.occurred_at = 9000u;
    check_equal(flowie_control_store_user_create(store, &command, &replay), TURBO_OK);
    check_equal(replay.revision, first.revision);
    check_true(replay.replayed);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 2u);

    command.principal_type = "service";
    replay = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(flowie_control_store_user_create(store, &command, &replay), TURBO_EBUSY);
    check_equal(replay.revision, 0u);

    control_store_close(store, path);
  }

  it("rejects a stale revision without partially creating a user") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t first = control_user_create_command("request-first", 1u);
    flowie_control_user_create_command_t stale = control_user_create_command("request-stale", 1u);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_user_view_t user = FLOWIE_CONTROL_USER_VIEW_INIT;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &first, &result), TURBO_OK);
    stale.principal_id = "device-stale";
    stale.occurred_at = 9000u;
    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(flowie_control_store_user_create(store, &stale, &result), TURBO_EBUSY);
    check_equal(flowie_control_store_user_get(store, "root-a", "device-stale", &user),
                 TURBO_ENOENT);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 2u);

    control_store_close(store, path);
  }

  it("generates a one-time credential and verifies only the matching secret") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_credential_issue_command_t issue =
        control_credential_issue_command("request-credential-generate", 2u);
    flowie_control_generated_credential_t generated = FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
    flowie_control_credential_verify_result_t verified =
        FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    char wrong_token[FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE];
    char zeros[FLOWIE_CONTROL_CREDENTIAL_TOKEN_CAPACITY] = {0};
    uint64_t revision = 0u;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    flowie_control_credential_issue_command_t stale =
        control_credential_issue_command("request-credential-stale", 1u);
    check_equal(flowie_control_store_credential_generate(store, &stale, &generated), TURBO_EBUSY);
    check_equal(generated.token_size, 0u);
    check_equal(generated.token, zeros, sizeof(generated.token));
    check_equal(flowie_control_store_credential_generate(store, &issue, &generated), TURBO_OK);
    check_equal(generated.revision, 3u);
    check_equal(generated.token_size, FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE);
    check_starts_with(generated.token, FLOWIE_CONTROL_CREDENTIAL_TOKEN_PREFIX);
    check_not_equal(generated.token, zeros, sizeof(generated.token));
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-7",
                                                        generated.token, generated.token_size,
                                                        &verified),
                 TURBO_OK);
    check_equal(verified.user_revision, 2u);
    check_equal(verified.credential_revision, 3u);

    memcpy(wrong_token, generated.token, sizeof(wrong_token));
    wrong_token[FLOWIE_CONTROL_CREDENTIAL_TOKEN_PREFIX_SIZE] ^= 0x01u;
    verified =
        (flowie_control_credential_verify_result_t)FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-7", wrong_token,
                                                        sizeof(wrong_token), &verified),
                 TURBO_EPERM);
    check_equal(verified.credential_revision, 0u);
    check_equal(flowie_control_store_credential_verify(store, "root-a", "missing-user",
                                                        generated.token, generated.token_size,
                                                        &verified),
                 TURBO_EPERM);

    issue.expected_revision = 99u;
    flowie_control_generated_credential_wipe(&generated);
    check_equal(generated.token, zeros, sizeof(generated.token));
    generated = (flowie_control_generated_credential_t)FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
    check_equal(flowie_control_store_credential_generate(store, &issue, &generated),
                 TURBO_EALREADY);
    check_equal(generated.token_size, 0u);
    check_equal(generated.token, zeros, sizeof(generated.token));
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 3u);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 3u);

    flowie_control_credential_wipe(wrong_token, sizeof(wrong_token));
    flowie_control_generated_credential_wipe(&generated);
    check_equal(generated.token, zeros, sizeof(generated.token));
    control_store_close(store, path);
  }

  it("rotates and revokes credentials without accepting an old or disabled secret") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_credential_issue_command_t issue =
        control_credential_issue_command("request-credential-generate", 2u);
    flowie_control_user_disable_command_t disable = FLOWIE_CONTROL_USER_DISABLE_COMMAND_INIT;
    flowie_control_generated_credential_t first = FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
    flowie_control_generated_credential_t rotated = FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
    flowie_control_credential_verify_result_t verified =
        FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    char old_token[FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE];
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    check_equal(flowie_control_store_credential_generate(store, &issue, &first), TURBO_OK);
    memcpy(old_token, first.token, sizeof(old_token));
    flowie_control_generated_credential_wipe(&first);
    issue = control_credential_issue_command("request-credential-rotate", 3u);
    check_equal(flowie_control_store_credential_rotate(store, &issue, &rotated), TURBO_OK);
    check_equal(rotated.revision, 4u);
    check_not_equal(rotated.token, old_token, sizeof(old_token));
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-7", old_token,
                                                        sizeof(old_token), &verified),
                 TURBO_EPERM);
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-7", rotated.token,
                                                        rotated.token_size, &verified),
                 TURBO_OK);
    check_equal(verified.credential_revision, 4u);

    check_equal(control_credential_revoke(store, "request-credential-revoke", 4u, &result),
                 TURBO_OK);
    check_equal(result.revision, 5u);
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-7", rotated.token,
                                                        rotated.token_size, &verified),
                 TURBO_EPERM);
    check_equal(control_credential_revoke(store, "request-credential-revoke", 0u, &result),
                 TURBO_OK);
    check_true(result.replayed);

    issue = control_credential_issue_command("request-credential-reactivate", 5u);
    flowie_control_generated_credential_wipe(&rotated);
    rotated = (flowie_control_generated_credential_t)FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
    check_equal(flowie_control_store_credential_rotate(store, &issue, &rotated), TURBO_OK);
    check_equal(rotated.revision, 6u);
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-7", rotated.token,
                                                        rotated.token_size, &verified),
                 TURBO_OK);

    disable.domain_id = "root-a";
    disable.principal_id = "device-7";
    disable.actor = "admin-1";
    disable.request_id = "request-disable-after-credential";
    disable.expected_revision = 6u;
    disable.occurred_at = 9000u;
    check_equal(flowie_control_store_user_disable(store, &disable, &result), TURBO_OK);
    check_equal(flowie_control_store_credential_verify(store, "root-a", "device-7", rotated.token,
                                                        rotated.token_size, &verified),
                 TURBO_EPERM);
    issue = control_credential_issue_command("request-disabled-user-rotate", 7u);
    check_equal(flowie_control_store_credential_rotate(store, &issue, &first), TURBO_EPERM);
    check_equal(first.token_size, 0u);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 7u);

    flowie_control_credential_wipe(old_token, sizeof(old_token));
    flowie_control_generated_credential_wipe(&first);
    flowie_control_generated_credential_wipe(&rotated);
    control_store_close(store, path);
  }

  it("serializes concurrent writers without partial commits") {
    enum { CONTROL_CONCURRENT_WRITERS = 2, CONTROL_CONCURRENT_ROUNDS = 8 };
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    uint64_t revision = 1u;
    size_t audit_count = 0u;

    for (unsigned int round = 0u; round < CONTROL_CONCURRENT_ROUNDS; ++round) {
      control_concurrent_group_create_t writes[CONTROL_CONCURRENT_WRITERS] = {0};
      turbo_thread_t threads[CONTROL_CONCURRENT_WRITERS] = {0};
      int thread_created[CONTROL_CONCURRENT_WRITERS] = {0};
      atomic_int ready;
      atomic_int go;
      size_t success_count = 0u;
      size_t busy_count = 0u;
      size_t loser = CONTROL_CONCURRENT_WRITERS;

      atomic_init(&ready, 0);
      atomic_init(&go, 0);
      for (size_t index = 0u; index < CONTROL_CONCURRENT_WRITERS; ++index) {
        writes[index].store = store;
        writes[index].expected_revision = revision;
        writes[index].ready = &ready;
        writes[index].go = &go;
        writes[index].result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
        writes[index].rc = TURBO_EIO;
        (void)snprintf(writes[index].group_id, sizeof(writes[index].group_id), "concurrent-%u-%zu",
                       round, index);
        (void)snprintf(writes[index].request_id, sizeof(writes[index].request_id),
                       "request-concurrent-%u-%zu", round, index);
        writes[index].rc =
            turbo_thread_create(&threads[index], control_concurrent_group_create, &writes[index]);
        check_equal(writes[index].rc, TURBO_OK);
        thread_created[index] = writes[index].rc == TURBO_OK;
      }
      if (!thread_created[0] || !thread_created[1]) {
        atomic_store_explicit(&go, 1, memory_order_release);
        for (size_t index = 0u; index < CONTROL_CONCURRENT_WRITERS; ++index) {
          if (!thread_created[index]) continue;
          check_equal(turbo_thread_join(&threads[index]), TURBO_OK);
          turbo_thread_destroy(&threads[index]);
        }
        break;
      }
      while (atomic_load_explicit(&ready, memory_order_acquire) != CONTROL_CONCURRENT_WRITERS)
        turbo_thread_yield();
      atomic_store_explicit(&go, 1, memory_order_release);
      for (size_t index = 0u; index < CONTROL_CONCURRENT_WRITERS; ++index) {
        check_equal(turbo_thread_join(&threads[index]), TURBO_OK);
        turbo_thread_destroy(&threads[index]);
        if (writes[index].rc == TURBO_OK) {
          ++success_count;
          check_equal(writes[index].result.revision, revision + 1u);
        } else if (writes[index].rc == TURBO_EBUSY) {
          ++busy_count;
          loser = index;
          check_equal(writes[index].result.revision, 0u);
        }
      }
      check_equal(success_count, 1u);
      check_equal(busy_count, 1u);
      if (loser >= CONTROL_CONCURRENT_WRITERS) break;
      check_equal(control_group_create(store, "root-a", writes[loser].group_id, NULL,
                                        writes[loser].request_id, revision + 1u,
                                        &writes[loser].result),
                   TURBO_OK);
      revision += 2u;
      check_equal(writes[loser].result.revision, revision);
    }
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 1u + 2u * CONTROL_CONCURRENT_ROUNDS);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 1u + 2u * CONTROL_CONCURRENT_ROUNDS);

    control_store_close(store, path);
  }

  it("disables a user once and replays the matching command") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t create = control_user_create_command("request-create", 1u);
    flowie_control_user_disable_command_t disable = FLOWIE_CONTROL_USER_DISABLE_COMMAND_INIT;
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_user_view_t user = FLOWIE_CONTROL_USER_VIEW_INIT;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &create, &result), TURBO_OK);
    disable.domain_id = "root-a";
    disable.principal_id = "device-7";
    disable.actor = "admin-1";
    disable.request_id = "request-disable";
    disable.expected_revision = 2u;
    disable.occurred_at = 5000u;
    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(flowie_control_store_user_disable(store, &disable, &result), TURBO_OK);
    check_equal(result.revision, 3u);
    check_false(result.replayed);
    check_equal(flowie_control_store_user_get(store, "root-a", "device-7", &user), TURBO_OK);
    check_false(user.enabled);
    check_equal(user.revision, 3u);

    disable.expected_revision = 0u;
    disable.occurred_at = 6000u;
    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(flowie_control_store_user_disable(store, &disable, &result), TURBO_OK);
    check_equal(result.revision, 3u);
    check_true(result.replayed);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 3u);

    disable.request_id = "request-disable-again";
    disable.expected_revision = 3u;
    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(flowie_control_store_user_disable(store, &disable, &result), TURBO_EALREADY);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 3u);

    control_store_close(store, path);
  }

  it("expands direct membership through ancestors and rejects cross-root parents") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_effective_groups_view_t groups = FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
    uint64_t revision = 0u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    check_equal(
        control_group_create(store, "root-a", "engineering", NULL, "request-eng", 2u, &result),
        TURBO_OK);
    check_equal(control_group_create(store, "root-a", "backend", "engineering", "request-backend",
                                      3u, &result),
                 TURBO_OK);
    check_equal(control_membership_add(store, "backend", "request-member", 4u, &result), TURBO_OK);
    check_equal(flowie_control_store_effective_groups(store, "root-a", "device-7", &groups),
                 TURBO_OK);
    check_equal(groups.group_count, 2u);
    check_equal(groups.groups[0], "engineering");
    check_equal(groups.groups[1], "backend");

    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(control_domain_create(store, "root-b", "request-root-b", 5u, &result), TURBO_OK);
    check_equal(control_group_create(store, "root-b", "foreign-child", "engineering",
                                      "request-cross-root", 6u, &result),
                 TURBO_ENOENT);
    check_equal(control_group_create(store, "root-a", "self-parent", "self-parent",
                                      "request-self-parent", 6u, &result),
                 TURBO_EINVAL);
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 6u);

    control_store_close(store, path);
  }

  it("removes direct membership and revokes inherited groups atomically") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_effective_groups_view_t groups = FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
    uint64_t revision = 0u;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    check_equal(
        control_group_create(store, "root-a", "engineering", NULL, "request-eng", 2u, &result),
        TURBO_OK);
    check_equal(control_group_create(store, "root-a", "backend", "engineering", "request-backend",
                                      3u, &result),
                 TURBO_OK);
    check_equal(control_membership_add(store, "backend", "request-member", 4u, &result), TURBO_OK);
    check_equal(control_membership_remove(store, "backend", "request-member-remove", 5u, &result),
                 TURBO_OK);
    check_equal(result.revision, 6u);
    check_equal(flowie_control_store_effective_groups(store, "root-a", "device-7", &groups),
                 TURBO_OK);
    check_equal(groups.group_count, 0u);

    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(control_membership_remove(store, "backend", "request-member-remove", 99u, &result),
                 TURBO_OK);
    check_true(result.replayed);
    check_equal(result.revision, 6u);
    check_equal(
        control_membership_remove(store, "backend", "request-member-remove-missing", 6u, &result),
        TURBO_ENOENT);
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 6u);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 6u);

    control_store_close(store, path);
  }

  it("deletes only unreferenced leaf groups and keeps the domain immutable") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_effective_groups_view_t groups = FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    check_equal(
        control_group_create(store, "root-a", "engineering", NULL, "request-eng", 2u, &result),
        TURBO_OK);
    check_equal(control_group_create(store, "root-a", "backend", "engineering", "request-backend",
                                      3u, &result),
                 TURBO_OK);
    check_equal(
        control_group_delete(store, "root-a", "engineering", "request-delete-eng", 4u, &result),
        TURBO_EBUSY);
    check_equal(control_membership_add(store, "backend", "request-member", 4u, &result), TURBO_OK);
    check_equal(
        control_group_delete(store, "root-a", "backend", "request-delete-backend", 5u, &result),
        TURBO_EBUSY);
    check_equal(control_membership_remove(store, "backend", "request-member-remove", 5u, &result),
                 TURBO_OK);
    check_equal(
        control_group_delete(store, "root-a", "backend", "request-delete-backend", 6u, &result),
        TURBO_OK);
    check_equal(result.revision, 7u);

    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(
        control_group_delete(store, "root-a", "backend", "request-delete-backend", 0u, &result),
        TURBO_OK);
    check_true(result.replayed);
    check_equal(result.revision, 7u);
    check_equal(
        control_group_delete(store, "root-a", "engineering", "request-delete-eng", 7u, &result),
        TURBO_OK);
    check_equal(result.revision, 8u);
    check_equal(
        control_group_delete(store, "root-a", "root-a", "request-delete-domain", 8u, &result),
        TURBO_EINVAL);
    check_equal(flowie_control_store_effective_groups(store, "root-a", "device-7", &groups),
                 TURBO_OK);
    check_equal(groups.group_count, 0u);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 8u);

    control_store_close(store, path);
  }

  it("permanently deletes an existing disabled leaf group") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_group_view_t groups[1] = {FLOWIE_CONTROL_GROUP_VIEW_INIT};
    size_t count = 0u;
    int has_more = 0;

    check_equal(control_group_create(store, "root-a", "testa", NULL, "request-testa", 1u,
                                      &result),
                 TURBO_OK);
    check_equal(control_store_mark_group_disabled(path, "root-a", "testa"), 0);
    check_equal(control_group_delete(store, "root-a", "testa", "request-delete-testa", 2u,
                                      &result),
                 TURBO_OK);
    check_equal(flowie_control_store_group_list(store, "root-a", NULL, groups, 1u, &count,
                                                 &has_more),
                 TURBO_OK);
    check_equal(count, 0u);
    check_false(has_more);

    control_store_close(store, path);
  }

  it("rejects a child deeper than the bounded group tree") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    char parent[FLOWIE_SECURITY_ID_MAX + 1u] = "";
    char group[FLOWIE_SECURITY_ID_MAX + 1u];
    char request[64];
    uint64_t revision = 1u;

    for (uint32_t depth = 0u; depth <= FLOWIE_CONTROL_GROUP_MAX_DEPTH; ++depth) {
      (void)snprintf(group, sizeof(group), "depth-%u", depth);
      (void)snprintf(request, sizeof(request), "request-depth-%u", depth);
      check_equal(control_group_create(store, "root-a", group, parent[0] ? parent : NULL, request,
                                        revision, &result),
                   TURBO_OK);
      ++revision;
      (void)snprintf(parent, sizeof(parent), "%s", group);
    }
    check_equal(control_group_create(store, "root-a", "too-deep", parent, "request-too-deep",
                                      revision, &result),
                 TURBO_ENOSPC);
    check_equal(revision, (uint64_t)FLOWIE_CONTROL_GROUP_MAX_DEPTH + 2u);

    control_store_close(store, path);
  }

  it("rejects membership whose effective group closure exceeds the security ABI") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_effective_groups_view_t groups = FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
    char group[64];
    char request[64];
    uint64_t revision = 2u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    for (uint32_t index = 0u; index <= FLOWIE_SECURITY_MAX_GROUPS; ++index) {
      (void)snprintf(group, sizeof(group), "branch-%u", index);
      (void)snprintf(request, sizeof(request), "request-group-%u", index);
      check_equal(
          control_group_create(store, "root-a", group, NULL, request, revision, &result),
          TURBO_OK);
      ++revision;
    }
    for (uint32_t index = 0u; index < FLOWIE_SECURITY_MAX_GROUPS; ++index) {
      (void)snprintf(group, sizeof(group), "branch-%u", index);
      (void)snprintf(request, sizeof(request), "request-member-%u", index);
      check_equal(control_membership_add(store, group, request, revision, &result), TURBO_OK);
      ++revision;
    }
    check_equal(flowie_control_store_effective_groups(store, "root-a", "device-7", &groups),
                 TURBO_OK);
    check_equal(groups.group_count, FLOWIE_SECURITY_MAX_GROUPS);
    (void)snprintf(group, sizeof(group), "branch-%u", FLOWIE_SECURITY_MAX_GROUPS);
    check_equal(control_membership_add(store, group, "request-member-overflow", revision, &result),
                 TURBO_ENOSPC);
    groups = (flowie_control_effective_groups_view_t)FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
    check_equal(flowie_control_store_effective_groups(store, "root-a", "device-7", &groups),
                 TURBO_OK);
    check_equal(groups.group_count, FLOWIE_SECURITY_MAX_GROUPS);

    control_store_close(store, path);
  }

  it("creates root-scoped roles and returns a bounded deterministic assignment snapshot") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_effective_roles_view_t roles = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
    uint64_t revision = 0u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    check_equal(control_role_create(store, "root-a", "writer", "request-role-writer", 2u, &result),
                 TURBO_OK);
    check_equal(control_role_create(store, "root-a", "reader", "request-role-reader", 3u, &result),
                 TURBO_OK);
    check_equal(control_user_role_add(store, "writer", "request-user-role-writer", 4u, &result),
                 TURBO_OK);
    check_equal(control_user_role_add(store, "reader", "request-user-role-reader", 5u, &result),
                 TURBO_OK);
    check_equal(flowie_control_store_effective_roles(store, "root-a", "device-7", &roles),
                 TURBO_OK);
    check_equal(roles.role_count, 2u);
    check_equal(roles.roles[0], "reader");
    check_equal(roles.roles[1], "writer");

    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(control_user_role_add(store, "writer", "request-user-role-writer", 99u, &result),
                 TURBO_OK);
    check_true(result.replayed);
    check_equal(result.revision, 5u);

    check_equal(control_domain_create(store, "root-b", "request-root-b", 6u, &result), TURBO_OK);
    check_equal(
        control_role_create(store, "root-b", "foreign", "request-role-foreign", 7u, &result),
        TURBO_OK);
    check_equal(control_user_role_add(store, "foreign", "request-cross-root-role", 8u, &result),
                 TURBO_ENOENT);
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 8u);

    control_store_close(store, path);
  }

  it("tombstones an assigned role without leaving an effective authorization") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_role_disable_command_t disable = FLOWIE_CONTROL_ROLE_DISABLE_COMMAND_INIT;
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_effective_roles_view_t roles = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    check_equal(control_role_create(store, "root-a", "writer", "request-role-writer", 2u, &result),
                 TURBO_OK);
    check_equal(control_user_role_add(store, "writer", "request-user-role-writer", 3u, &result),
                 TURBO_OK);
    disable.domain_id = "root-a";
    disable.role_id = "writer";
    disable.actor = "admin-1";
    disable.request_id = "request-role-disable";
    disable.expected_revision = 4u;
    disable.occurred_at = 7000u;
    check_equal(flowie_control_store_role_disable(store, &disable, &result), TURBO_OK);
    check_equal(result.revision, 5u);
    check_equal(flowie_control_store_effective_roles(store, "root-a", "device-7", &roles),
                 TURBO_OK);
    check_equal(roles.role_count, 0u);

    disable.expected_revision = 0u;
    disable.occurred_at = 8000u;
    result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    check_equal(flowie_control_store_role_disable(store, &disable, &result), TURBO_OK);
    check_true(result.replayed);
    check_equal(result.revision, 5u);
    check_equal(control_user_role_remove(store, "writer", "request-user-role-remove", 5u, &result),
                 TURBO_OK);
    check_equal(result.revision, 6u);
    check_equal(control_user_role_remove(store, "writer", "request-user-role-remove", 0u, &result),
                 TURBO_OK);
    check_true(result.replayed);
    check_equal(result.revision, 6u);
    check_equal(control_user_role_add(store, "writer", "request-disabled-role", 6u, &result),
                 TURBO_EPERM);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 6u);

    control_store_close(store, path);
  }

  it("rolls back a user-role assignment beyond the security ABI capacity") {
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_effective_roles_view_t roles = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
    char role[32];
    char request[64];
    uint64_t revision = 2u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    for (uint32_t index = 0u; index <= FLOWIE_SECURITY_MAX_ROLES; ++index) {
      (void)snprintf(role, sizeof(role), "role-%u", index);
      (void)snprintf(request, sizeof(request), "request-role-%u", index);
      check_equal(control_role_create(store, "root-a", role, request, revision, &result),
                   TURBO_OK);
      ++revision;
    }
    for (uint32_t index = 0u; index < FLOWIE_SECURITY_MAX_ROLES; ++index) {
      (void)snprintf(role, sizeof(role), "role-%u", index);
      (void)snprintf(request, sizeof(request), "request-user-role-%u", index);
      check_equal(control_user_role_add(store, role, request, revision, &result), TURBO_OK);
      ++revision;
    }
    (void)snprintf(role, sizeof(role), "role-%u", FLOWIE_SECURITY_MAX_ROLES);
    check_equal(
        control_user_role_add(store, role, "request-user-role-overflow", revision, &result),
        TURBO_ENOSPC);
    check_equal(flowie_control_store_effective_roles(store, "root-a", "device-7", &roles),
                 TURBO_OK);
    check_equal(roles.role_count, FLOWIE_SECURITY_MAX_ROLES);
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 19u);

    control_store_close(store, path);
  }

  it("validates canonical policy drafts, references, MQTT filters, and bounded listing") {
    static const char valid_rule[] =
        "user device-7 allow {\n"
        "  read topic root-a/groups/operators/devices/%u/event\n"
        "}";
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_policy_validation_t validation = FLOWIE_CONTROL_POLICY_VALIDATION_INIT;
    flowie_control_policy_rule_view_t rules[1] = {FLOWIE_CONTROL_POLICY_RULE_VIEW_INIT};
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    size_t count = 0u;
    int has_more = 0;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    check_equal(control_group_create(store, "root-a", "operators", NULL, "request-group", 2u,
                                      &result),
                 TURBO_OK);
    check_equal(control_policy_rule_put(store, 10u, valid_rule, "request-policy-put", 3u, &result),
                 TURBO_OK);
    check_equal(result.revision, 4u);
    check_equal(
        flowie_control_store_policy_rule_list(store, "root-a", 0u, 0, rules, 1u, &count, &has_more),
        TURBO_OK);
    check_equal(count, 1u);
    check_false(has_more);
    check_equal(rules[0].ordinal, 10u);
    check_equal(rules[0].rule_line, valid_rule);
    check_equal(flowie_control_store_policy_validate(store, "root-a", &validation), TURBO_OK);
    check_equal(validation.rule_count, 2u);
    check_equal(validation.deny_rule_count, 0u);

    check_equal(control_policy_rule_put(
                     store, 11u,
                     "user device-7 allow { write topic root-a/groups/operators/devices/%u/event }",
                     "request-policy-noncanonical", 4u, &result),
                 TURBO_EPROTO);
    check_equal(control_policy_rule_put(
                     store, 11u,
                     "user device-7 allow {\n  read topic other-a/operators/devices/%u/event\n}",
                     "request-policy-bad-filter", 4u, &result),
                 TURBO_EPROTO);
    check_equal(control_policy_rule_put(
                     store, 11u,
                     "user missing allow {\n  read topic root-a/groups/operators/devices/%u/event\n}",
                     "request-policy-missing-principal", 4u, &result),
                 TURBO_ENOENT);
    check_equal(flowie_control_store_revision(store, &validation.store_revision), TURBO_OK);
    check_equal(validation.store_revision, 4u);

    control_store_close(store, path);
  }

  it("validates typed subjects and replaces one rule by subject key") {
    static const char role_rule[] =
        "role shared allow {\n"
        "  read topic root-a/commands/#\n"
        "}";
    static const char group_rule[] =
        "group shared allow {\n"
        "  write topic root-a/telemetry/%u/event\n"
        "}";
    static const char user_rule[] = "user shared allow";
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user =
        control_user_create_command("request-shared-user", 1u);
    flowie_control_role_disable_command_t disable =
        FLOWIE_CONTROL_ROLE_DISABLE_COMMAND_INIT;
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_policy_validation_t validation = FLOWIE_CONTROL_POLICY_VALIDATION_INIT;

    user.principal_id = "shared";
    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    check_equal(control_group_create(store, "root-a", "shared", NULL, "request-shared-group",
                                      2u, &result),
                TURBO_OK);
    check_equal(control_role_create(store, "root-a", "shared", "request-shared-role", 3u,
                                     &result),
                TURBO_OK);
    check_equal(control_role_create(store, "root-a", "disabled", "request-disabled-role", 4u,
                                     &result),
                TURBO_OK);
    disable.domain_id = "root-a";
    disable.role_id = "disabled";
    disable.actor = "admin-1";
    disable.request_id = "request-disable-role";
    disable.expected_revision = 5u;
    disable.occurred_at = 9800u;
    check_equal(flowie_control_store_role_disable(store, &disable, &result), TURBO_OK);

    check_equal(control_policy_rule_put(store, 10u, role_rule, "request-role-rule", 6u,
                                         &result),
                TURBO_OK);
    check_equal(control_policy_rule_put(store, 20u, group_rule, "request-group-rule", 7u,
                                         &result),
                TURBO_OK);
    check_equal(control_policy_rule_put(store, 30u, user_rule, "request-user-rule", 8u,
                                         &result),
                TURBO_OK);
    check_equal(flowie_control_store_policy_validate(store, "root-a", &validation), TURBO_OK);
    check_equal(validation.rule_count, 5u);

    check_equal(control_policy_rule_put(store, 40u, role_rule, "request-duplicate-role", 9u,
                                         &result),
                TURBO_OK);
    check_equal(control_policy_rule_put(store, 40u, "role missing allow",
                                         "request-missing-role", 10u, &result),
                TURBO_ENOENT);
    check_equal(control_policy_rule_put(store, 40u, "group missing allow",
                                         "request-missing-group", 10u, &result),
                TURBO_ENOENT);
    check_equal(control_policy_rule_put(store, 40u, "user missing allow",
                                         "request-missing-user", 10u, &result),
                TURBO_ENOENT);
    check_equal(control_policy_rule_put(store, 40u, "role disabled allow",
                                         "request-disabled-role-rule", 10u, &result),
                TURBO_EPERM);

    control_store_close(store, path);
  }

  it("prevents tombstoning subjects referenced by draft or published policy") {
    static const char principal_rule[] =
        "user device-7 allow {\n"
        "  read topic root-a/groups/operators/devices/%u/event\n"
        "}";
    static const char group_rule[] =
        "group operators allow {\n"
        "  read topic root-a/operations/#\n"
        "}";
    static const char role_rule[] =
        "role reader allow {\n"
        "  read topic root-a/reports/#\n"
        "}";
    static const char replacement_rule[] =
        "user device-8 allow {\n"
        "  write topic root-a/groups/public/devices/%c/event\n"
        "}";
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_user_create_command_t user = control_user_create_command("request-user", 1u);
    flowie_control_user_create_command_t replacement_user =
        control_user_create_command("request-replacement-user", 2u);
    flowie_control_user_disable_command_t user_disable = FLOWIE_CONTROL_USER_DISABLE_COMMAND_INIT;
    flowie_control_role_disable_command_t role_disable = FLOWIE_CONTROL_ROLE_DISABLE_COMMAND_INIT;
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_policy_publish_result_t published = FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
    uint64_t revision = 0u;
    size_t audit_count = 0u;

    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);
    replacement_user.principal_id = "device-8";
    check_equal(flowie_control_store_user_create(store, &replacement_user, &result), TURBO_OK);
    check_equal(control_group_create(store, "root-a", "operators", NULL, "request-group",
                                      3u, &result),
                 TURBO_OK);
    check_equal(control_group_create(store, "root-a", "public", NULL, "request-public-group",
                                      4u, &result),
                 TURBO_OK);
    check_equal(control_role_create(store, "root-a", "reader", "request-role", 5u, &result),
                 TURBO_OK);
    check_equal(control_policy_rule_put(store, 10u, principal_rule, "request-principal-rule", 6u,
                                         &result),
                 TURBO_OK);
    check_equal(control_policy_rule_put(store, 20u, group_rule, "request-group-rule", 7u,
                                         &result),
                TURBO_OK);
    check_equal(control_policy_rule_put(store, 30u, role_rule, "request-role-rule", 8u,
                                         &result),
                TURBO_OK);

    user_disable.domain_id = "root-a";
    user_disable.principal_id = "device-7";
    user_disable.actor = "admin-1";
    user_disable.request_id = "request-disable-user-referenced";
    user_disable.expected_revision = 9u;
    user_disable.occurred_at = 10000u;
    role_disable.domain_id = "root-a";
    role_disable.role_id = "reader";
    role_disable.actor = "admin-1";
    role_disable.request_id = "request-disable-role-referenced";
    role_disable.expected_revision = 9u;
    role_disable.occurred_at = 10001u;
    check_equal(flowie_control_store_user_disable(store, &user_disable, &result), TURBO_EBUSY);
    check_equal(control_group_delete(store, "root-a", "operators",
                                      "request-delete-group-referenced", 9u, &result),
                 TURBO_EBUSY);
    check_equal(flowie_control_store_role_disable(store, &role_disable, &result), TURBO_EBUSY);

    check_equal(control_policy_publish(store, "request-publish-subject-rules", 9u, 20000u,
                                        &published),
                 TURBO_OK);
    check_equal(control_policy_rule_delete(store, 10u, "request-delete-principal-rule", 10u,
                                            &result),
                 TURBO_OK);
    check_equal(control_policy_rule_delete(store, 20u, "request-delete-group-rule", 11u,
                                            &result),
                TURBO_OK);
    check_equal(control_policy_rule_delete(store, 30u, "request-delete-role-rule", 12u,
                                            &result),
                TURBO_OK);
    user_disable.expected_revision = 13u;
    check_equal(flowie_control_store_user_disable(store, &user_disable, &result), TURBO_EBUSY);
    check_equal(control_group_delete(store, "root-a", "operators",
                                      "request-delete-group-published", 13u, &result),
                 TURBO_EBUSY);
    role_disable.expected_revision = 13u;
    check_equal(flowie_control_store_role_disable(store, &role_disable, &result), TURBO_EBUSY);

    check_equal(control_policy_rule_put(store, 40u, replacement_rule, "request-replacement-rule",
                                         13u, &result),
                 TURBO_OK);
    check_equal(control_policy_publish(store, "request-publish-replacement", 14u, 21000u,
                                        &published),
                 TURBO_OK);
    check_equal(control_group_delete(store, "root-a", "operators", "request-delete-group", 15u,
                                      &result),
                 TURBO_OK);
    role_disable.request_id = "request-disable-role";
    role_disable.expected_revision = 16u;
    role_disable.occurred_at = 10002u;
    check_equal(flowie_control_store_role_disable(store, &role_disable, &result), TURBO_OK);
    user_disable.request_id = "request-disable-user";
    user_disable.expected_revision = 17u;
    user_disable.occurred_at = 10003u;
    check_equal(flowie_control_store_user_disable(store, &user_disable, &result), TURBO_OK);
    check_equal(control_group_delete(store, "root-a", "public",
                                      "request-delete-resource-segment-group", 18u, &result),
                TURBO_OK);
    check_equal(flowie_control_store_revision(store, &revision), TURBO_OK);
    check_equal(revision, 19u);
    check_equal(flowie_control_store_audit_count(store, &audit_count), TURBO_OK);
    check_equal(audit_count, 19u);

    control_store_close(store, path);
  }

  it("publishes an atomic versioned bundle through the Flowie SQLite provider") {
    static const char first_rule[] =
        "user device-7 allow {\n"
        "  deny read topic root-a/groups/operators/devices/%u/private\n"
        "}";
    static const char second_rule[] =
        "user device-8 allow {\n"
        "  read topic root-a/groups/operators/devices/%c/event\n"
        "}";
    char *path = NULL;
    flowie_control_store_t *store = control_store_open(&path);
    flowie_control_command_result_t put = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_policy_publish_result_t published = FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
    flowie_control_policy_status_t status = FLOWIE_CONTROL_POLICY_STATUS_INIT;
    flowie_security_sqlite_config_t provider_config = FLOWIE_SECURITY_SQLITE_CONFIG_INIT;
    flowie_security_sqlite_provider_t *provider = NULL;
    const flowie_security_policy_provider_t *interface = NULL;
    flowie_security_policy_bundle_t bundle = FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
    flowie_security_policy_bundle_t repository_bundle =
        FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
    flowie_control_user_create_command_t first_user = control_user_create_command("request-user-7", 1u);
    flowie_control_user_create_command_t second_user = control_user_create_command("request-user-8", 2u);

    check_equal(flowie_control_store_user_create(store, &first_user, &put), TURBO_OK);
    second_user.principal_id = "device-8";
    check_equal(flowie_control_store_user_create(store, &second_user, &put), TURBO_OK);
    check_equal(control_group_create(store, "root-a", "operators", NULL, "request-operators", 3u,
                                      &put),
                 TURBO_OK);
    check_equal(control_policy_rule_put(store, 20u, first_rule, "request-policy-first", 4u, &put),
                 TURBO_OK);
    check_equal(
        control_policy_rule_put(store, 40u, second_rule, "request-policy-second", 5u, &put),
        TURBO_OK);
    check_equal(control_policy_publish(store, "request-policy-publish", 6u, 20000u, &published),
                 TURBO_OK);
    check_equal(published.revision, 7u);
    check_equal(published.policy_version, 1u);
    check_false(published.replayed);

    check_equal(flowie_control_store_policy_bundle_load(store, "root-a", 0u,
                                                         &repository_bundle),
                 TURBO_OK);
    check_equal(repository_bundle.policy_version, 1u);
    check_equal(repository_bundle.expires_at, 20000u);
    check_equal(repository_bundle.rule_count, 4u);
    check_equal(repository_bundle.rules[0].pattern, "");
    check_equal(repository_bundle.rules[1].pattern,
                 "root-a/groups/operators/devices/%u/private");
    check_equal(repository_bundle.rules[2].pattern, "");
    check_equal(repository_bundle.rules[3].pattern,
                 "root-a/groups/operators/devices/%c/event");
    flowie_control_store_policy_bundle_release(&repository_bundle);

    repository_bundle =
        (flowie_security_policy_bundle_t)FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
    check_equal(flowie_control_store_policy_bundle_load(store, "root-a", 1u,
                                                         &repository_bundle),
                 TURBO_OK);
    check_equal(repository_bundle.rule_count, 4u);
    flowie_control_store_policy_bundle_release(&repository_bundle);
    check_equal(flowie_control_store_policy_bundle_load(store, "root-a", 2u,
                                                         &repository_bundle),
                 TURBO_ENOENT);

    provider_config.database_path = path;
    provider_config.namespace_name = "root-a";
    check_equal(flowie_security_sqlite_provider_create(&provider_config, &provider), TURBO_OK);
    interface = flowie_security_sqlite_provider_interface(provider);
    check_not_null(interface);
    check_equal(interface->load(interface->ctx, 1u, &bundle), TURBO_OK);
    check_equal(bundle.policy_version, 1u);
    check_equal(bundle.expires_at, 20000u);
    check_equal(bundle.rule_count, 4u);
    check_equal(bundle.rules[1].pattern, "root-a/groups/operators/devices/%u/private");
    check_equal(bundle.rules[3].pattern, "root-a/groups/operators/devices/%c/event");
    interface->release(interface->ctx, &bundle);

    published = (flowie_control_policy_publish_result_t)FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
    check_equal(control_policy_publish(store, "request-policy-publish", 0u, 20000u, &published),
                 TURBO_OK);
    check_true(published.replayed);
    check_equal(published.revision, 7u);
    check_equal(published.policy_version, 1u);
    check_equal(control_policy_publish(store, "request-policy-publish", 0u, 21000u, &published),
                 TURBO_EBUSY);
    check_equal(control_policy_publish(store, "request-policy-stale", 6u, 21000u, &published),
                 TURBO_EBUSY);
    check_equal(flowie_control_store_policy_status(store, "root-a", &status), TURBO_OK);
    check_equal(status.store_revision, 7u);
    check_equal(status.policy_version, 1u);
    check_equal(status.draft_rule_count, 2u);
    check_equal(status.published_rule_count, 4u);

    bundle = (flowie_security_policy_bundle_t)FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
    check_equal(interface->load(interface->ctx, 1u, &bundle), TURBO_OK);
    check_equal(bundle.rule_count, 4u);
    interface->release(interface->ctx, &bundle);
    flowie_security_sqlite_provider_destroy(provider);
    control_store_close(store, path);
  }
}
