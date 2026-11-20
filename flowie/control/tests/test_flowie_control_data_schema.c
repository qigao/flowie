#include "flowie_control_data_schema_internal.h"
#include "flowie_control_test_turbodb.h"

#include "tinytest.h"

#include <stdint.h>

static int schema_scalar(flowie_control_database_t *database, const char *sql, int64_t *out) {
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

spec("Flowie Control data manifest schema") {
  it("creates the versioned declarative Domain archive with enforced references") {
    static const char orphan_membership[] =
        "INSERT INTO flowie_control_data_membership(principal_id,group_id) "
        "VALUES('missing','missing')";
    flowie_control_database_t *database = NULL;
    const char *schema;
    size_t schema_size = 0u;
    int64_t value = 0;

    check_equal(flowie_control_test_database_open(":memory:", &database), FLOWIE_CONTROL_DB_OK);
    schema = flowie_control_data_schema_sql(&schema_size);
    check_not_null(schema);
    check(schema_size > 0u);
    check_equal(flowie_control_database_exec(database, schema, NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_OK);

    check_equal(schema_scalar(database,
                              "SELECT format_version FROM flowie_control_data_metadata "
                              "WHERE singleton=1",
                              &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)1);
    check_equal(schema_scalar(database,
                              "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                              "name LIKE 'flowie_control_data_%'",
                              &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)9);
    check_equal(schema_scalar(database,
                              "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                              "name='flowie_control_data_policy_ordinal_idx'",
                              &value),
                FLOWIE_CONTROL_DB_OK);
    check_equal(value, (int64_t)1);
    check_equal(flowie_control_database_exec(database, orphan_membership, NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_CONSTRAINT);
    check_equal(flowie_control_database_close(database), FLOWIE_CONTROL_DB_OK);
  }
}
