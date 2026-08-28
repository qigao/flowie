#include "flowie_control_database_internal.h"

#include "tinytest.h"

#include <string.h>

spec("Flowie TurboDB control database adapter") {
  it("owns binds, typed results, changes, and transaction rollback") {
    flowie_control_database_t *database = NULL;
    flowie_control_statement_t *statement = NULL;
    static const unsigned char payload[] = {0x00u, 0x7fu, 0xffu};

    check_equal(flowie_control_database_open_sqlite(":memory:", 1000, &database),
                FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_exec(database,
                                             "create table item(id integer, name text, data blob)",
                                             NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_prepare(database,
                                                "insert into item(id,name,data) values(?1,?2,?3)",
                                                -1, &statement, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_bind_int64(statement, 1, 7), FLOWIE_CONTROL_DB_OK);
    check_equal(
        flowie_control_database_bind_text(statement, 2, "Alice", -1, FLOWIE_CONTROL_DB_TRANSIENT),
        FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_bind_blob(statement, 3, payload, (int)sizeof(payload),
                                                  FLOWIE_CONTROL_DB_TRANSIENT),
                FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_step(statement), FLOWIE_CONTROL_DB_DONE);
    check_equal(flowie_control_database_changes(database), 1);
    check_equal(flowie_control_database_finalize(statement), FLOWIE_CONTROL_DB_OK);
    statement = NULL;

    check_equal(flowie_control_database_prepare(database, "select id,name,data from item", -1,
                                                &statement, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_step(statement), FLOWIE_CONTROL_DB_ROW);
    check_equal(flowie_control_database_column_type(statement, 0), FLOWIE_CONTROL_DB_INTEGER);
    check_equal(flowie_control_database_column_int64(statement, 0), (int64_t)7);
    check_equal(flowie_control_database_column_type(statement, 1), FLOWIE_CONTROL_DB_TEXT);
    check_true(strcmp((const char *)flowie_control_database_column_text(statement, 1), "Alice") ==
               0);
    check_equal(flowie_control_database_column_type(statement, 2), FLOWIE_CONTROL_DB_BLOB);
    check_equal(flowie_control_database_column_bytes(statement, 2), (int)sizeof(payload));
    check_true(
        memcmp(flowie_control_database_column_blob(statement, 2), payload, sizeof(payload)) == 0);
    check_equal(flowie_control_database_step(statement), FLOWIE_CONTROL_DB_DONE);
    check_equal(flowie_control_database_finalize(statement), FLOWIE_CONTROL_DB_OK);
    statement = NULL;

    check_equal(flowie_control_database_exec(database, "begin immediate", NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(
        flowie_control_database_exec(database, "delete from item where id=7", NULL, NULL, NULL),
        FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_exec(database, "rollback", NULL, NULL, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_prepare(database, "select count(*) from item", -1,
                                                &statement, NULL),
                FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_step(statement), FLOWIE_CONTROL_DB_ROW);
    check_equal(flowie_control_database_column_int64(statement, 0), (int64_t)1);

    check_equal(flowie_control_database_finalize(statement), FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_database_close(database), FLOWIE_CONTROL_DB_OK);
  }
}
