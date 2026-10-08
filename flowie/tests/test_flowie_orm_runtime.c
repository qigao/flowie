#include "flowie_orm_flow_internal.h"
#include "tinytest.h"

#include <string.h>

spec("Flowie TurboDB driver runtime") {
  static orm_config_t config;
  static orm_option_t options[FLOWIE_ORM_MAX_OPTIONS + 1u];
  static orm_connection_t *connection;
  static orm_connection_t *second;
  static orm_query_t *query;
  static orm_result_t *result;
  static orm_error_t error;

  before_each() {
    orm_config(&config);
    orm_error_init(&error);
    memset(options, 0, sizeof(options));
    config.driver = orm_view("sqlite");
    config.options = options;
    config.option_count = 1u;
    options[0].keyword = orm_view("filename");
    options[0].value = orm_view(":memory:");
    connection = second = NULL;
    query = NULL;
    result = NULL;
  }

  after_each() {
    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_connection_release(second);
    orm_connection_release(connection);
  }

  it("loads the SDK default and keeps independent connections usable") {
    uint64_t rows = 0u;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    check_equal(flowie_orm_connect(&config, &second, &error), ORM_STATUS_OK);
    orm_connection_release(connection);
    connection = NULL;
    check_equal(orm_raw(second, orm_view("SELECT 42"), &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, 1u);
  }

  it("retains the runtime and module while a query owns the last connection reference") {
    uint64_t rows = 0u;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("SELECT 42"), &query, &error), ORM_STATUS_OK);
    orm_connection_release(connection);
    connection = NULL;
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, 1u);
  }

  it("consumes an explicit module option before passing options to SQLite") {
    const char path_view[] = FLOWIE_TEST_SQLITE_MODULE "ignored";
    options[1].keyword = orm_view(FLOWIE_ORM_DRIVER_MODULE_OPTION);
    options[1].value.data = path_view;
    options[1].value.len = sizeof(FLOWIE_TEST_SQLITE_MODULE) - 1u;
    config.option_count = 2u;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    /* Callers can discard all borrowed configuration after open. */
    memset(options, 0, sizeof(options));
    memset(&config, 0, sizeof(config));
    check_equal(orm_raw(connection, orm_view("SELECT 42"), &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
  }

  it("does not fall back when the explicit module is missing") {
    orm_status_t status;
    options[1].keyword = orm_view(FLOWIE_ORM_DRIVER_MODULE_OPTION);
    options[1].value = orm_view(FLOWIE_TEST_SQLITE_MODULE ".missing");
    config.option_count = 2u;
    status = flowie_orm_connect(&config, &connection, &error);
    /* 2.3.1's native loader may report a generic module load error. */
    check_true(status == ORM_STATUS_DRIVER_MODULE_NOT_FOUND || status == ORM_STATUS_DRIVER_LOAD_ERROR);
    check_null(connection);
    check_equal(error.status, status);
  }

  it("rejects relative empty duplicate and embedded-NUL module paths") {
    const char embedded_nul[] = FLOWIE_TEST_SQLITE_MODULE "\0suffix";
    options[1].keyword = orm_view(FLOWIE_ORM_DRIVER_MODULE_OPTION);
    options[1].value = orm_view("turbodb_driver_sqlite");
    config.option_count = 2u;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_null(connection);
    options[1].value = orm_view("");
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_null(connection);
    options[1].value.data = embedded_nul;
    options[1].value.len = sizeof(embedded_nul) - 1u;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_null(connection);
    options[1].value = orm_view(FLOWIE_TEST_SQLITE_MODULE);
    options[2] = options[1];
    config.option_count = 3u;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_null(connection);
  }

  it("rejects a module whose canonical identity differs from the selected driver") {
    options[1].keyword = orm_view(FLOWIE_ORM_DRIVER_MODULE_OPTION);
    options[1].value = orm_view(FLOWIE_TEST_SQLITE_MODULE);
    config.option_count = 2u;
    config.driver = orm_view("postgres");
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_DRIVER_ID_MISMATCH);
    check_null(connection);
    check_equal(error.status, ORM_STATUS_DRIVER_ID_MISMATCH);
  }

  it("loads all four SDK drivers and the postgres alias before validating connect limits") {
    const char *drivers[] = {"sqlite", "postgresql", "postgres", "mysql", "tidesdb"};
    /* Invalid generic limits fail before a backend can contact a database. */
    config.max_parameters = 0u;
    for (size_t index = 0u; index < sizeof(drivers) / sizeof(drivers[0]); ++index) {
      config.driver = orm_view(drivers[index]);
      check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT);
      check_null(connection);
      check_contains(error.message, "configuration limits");
    }
  }

  it("rejects unknown drivers and invalid or oversized option arrays before loading") {
    config.driver = orm_view("unregistered");
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_DRIVER_NOT_REGISTERED);
    check_null(connection);
    config.driver = orm_view("sqlite");
    config.options = NULL;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_null(connection);
    config.options = options;
    config.option_count = FLOWIE_ORM_MAX_OPTIONS + 1u;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_LIMIT_EXCEEDED);
    check_null(connection);
    config.option_count = 1u;
    options[0].keyword.data = NULL;
    check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_null(connection);
  }

  it("unwinds failed connects and repeatedly reloads after final release") {
    for (unsigned attempt = 0u; attempt < 8u; ++attempt) {
      options[0].keyword = orm_view("unknown_sqlite_option");
      check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT);
      check_null(connection);
      check_not_equal(error.message[0], '\0');
      options[0].keyword = orm_view("filename");
      check_equal(flowie_orm_connect(&config, &connection, &error), ORM_STATUS_OK);
      orm_connection_release(connection);
      connection = NULL;
    }
  }
}
