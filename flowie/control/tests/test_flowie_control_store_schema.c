#include "flowie_control_store_schema_internal.h"
#include "flowie_control_test_turbodb.h"

#include "tinytest.h"

#include <stdint.h>
#include <string.h>

static int store_schema_scalar(flowie_control_database_t *database, const char *sql,
                               int64_t *out) {
  flowie_control_statement_t *statement = NULL;
  int status;
  if (out) *out = 0;
  if (!database || !sql || !out) return FLOWIE_CONTROL_DB_MISMATCH;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return status;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_ROW &&
      flowie_control_database_column_type(statement, 0) == FLOWIE_CONTROL_DB_INTEGER) {
    *out = flowie_control_database_column_int64(statement, 0);
    status = flowie_control_database_step(statement);
  }
  (void)flowie_control_database_finalize(statement);
  return status == FLOWIE_CONTROL_DB_DONE ? FLOWIE_CONTROL_DB_OK : status;
}

static int store_schema_text(flowie_control_database_t *database, const char *sql,
                             const char *expected) {
  flowie_control_statement_t *statement = NULL;
  const unsigned char *value;
  int size;
  int status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return status;
  status = flowie_control_database_step(statement);
  value = status == FLOWIE_CONTROL_DB_ROW ? flowie_control_database_column_text(statement, 0)
                                          : NULL;
  size = value ? flowie_control_database_column_bytes(statement, 0) : -1;
  if (!value || size < 0 || strlen(expected) != (size_t)size ||
      memcmp(value, expected, (size_t)size) != 0)
    status = FLOWIE_CONTROL_DB_MISMATCH;
  else if (flowie_control_database_step(statement) != FLOWIE_CONTROL_DB_DONE)
    status = FLOWIE_CONTROL_DB_ERROR;
  else
    status = FLOWIE_CONTROL_DB_OK;
  (void)flowie_control_database_finalize(statement);
  return status;
}

spec("Flowie Control persistent store schema") {
  it("creates the complete versioned schema with seeds indexes and cascade constraints") {
    static const char fixture[] =
        "INSERT INTO flowie_control_domain(domain_id) VALUES('root-a');"
        "INSERT INTO flowie_control_user(domain_id,principal_id,principal_type,enabled,revision,"
        "created_at,updated_at) VALUES('root-a','operator','user',1,1,1,1);"
        "INSERT INTO flowie_control_management_session(token_digest,domain_id,principal_id,csrf,"
        "expires_at,issued_sequence,last_used) VALUES("
        "X'000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',"
        "'root-a','operator','cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',"
        "100,1,1);"
        "INSERT INTO flowie_control_published_bundle(namespace_name,policy_version,expires_at) "
        "VALUES('root-a',1,100);"
        "INSERT INTO flowie_control_published_rule(namespace_name,ordinal,rule_line) "
        "VALUES('root-a',0,'user operator allow { read topic root-a/event }');";
    flowie_control_database_t *database = NULL;
    const char *schema;
    size_t schema_size = 0u;
    int64_t value = 0;

    check_equal(flowie_control_test_database_open(":memory:", &database), FLOWIE_CONTROL_DB_OK);
    schema = flowie_control_store_schema_sql("sqlite", &schema_size);
    check_not_null(schema);
    check(schema_size >= sizeof("BEGIN;\nCOMMIT;\n") - 1u);
    check_equal(memcmp(schema, "BEGIN;\n", sizeof("BEGIN;\n") - 1u), 0);
    check_equal(memcmp(schema + schema_size - (sizeof("COMMIT;\n") - 1u),
                       "COMMIT;\n", sizeof("COMMIT;\n") - 1u),
                0);
    check_equal(flowie_control_database_exec(database, schema, NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_null(flowie_control_store_schema_sql("unknown", NULL));

    check_equal(store_schema_scalar(
                    database,
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                    "name LIKE 'flowie_control_%'",
                    &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)16);
    check_equal(store_schema_scalar(
                    database,
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name IN("
                    "'flowie_control_management_session_principal_idx',"
                    "'flowie_control_management_session_lru_idx',"
                    "'flowie_control_management_session_expiry_idx')",
                    &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)3);
    check_equal(store_schema_scalar(
                    database,
                    "SELECT version FROM flowie_control_schema_version WHERE singleton=1",
                    &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)7);
    check_equal(store_schema_text(
                    database,
                    "SELECT fingerprint FROM flowie_control_schema_version WHERE singleton=1",
                    "flowie-control-persistent-session-schema-v7-20260829"),
                FLOWIE_CONTROL_DB_OK);
    check_equal(store_schema_scalar(database,
                                    "SELECT revision FROM flowie_control_meta WHERE singleton=1",
                                    &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)0);
    check_equal(store_schema_scalar(
                    database,
                    "SELECT value FROM flowie_control_management_session_sequence WHERE singleton=1",
                    &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)0);

    check_equal(flowie_control_database_exec(database, fixture, NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_exec(
                    database,
                    "DELETE FROM flowie_control_user WHERE domain_id='root-a' AND "
                    "principal_id='operator'",
                    NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(store_schema_scalar(database,
                                    "SELECT COUNT(*) FROM flowie_control_management_session",
                                    &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)0);
    check_equal(flowie_control_database_exec(
                    database,
                    "DELETE FROM flowie_control_published_bundle WHERE namespace_name='root-a'",
                    NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(store_schema_scalar(database,
                                    "SELECT COUNT(*) FROM flowie_control_published_rule", &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)0);
    check_equal(flowie_control_database_close(database), FLOWIE_CONTROL_DB_OK);
  }
}
