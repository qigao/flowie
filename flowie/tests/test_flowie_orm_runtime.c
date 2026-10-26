#include "flowie_orm_flow_internal.h"
#include "tinytest.h"

#include <string.h>

/* Test-only trusted Windows DLL location, sourced from the EXACT installed
 * TurboDB SDK CMake path. CMeta's secure explicit-path loader ignores PATH.
 * Register only the SDK's bin directory; no dependency search fallback. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#if !defined(FLOWIE_TEST_TURBODB_RUNTIME_DIR) || !defined(FLOWIE_TEST_SALTS_RUNTIME_DIR)
#error "Windows ORM Plugin test requires exact TurboDB and Salts SDK runtime paths"
#endif
static DLL_DIRECTORY_COOKIE flowie_sdk_turbodb_cookie;
static DLL_DIRECTORY_COOKIE flowie_sdk_salts_cookie;

static int flowie_test_trust_sdk_bin(const char *path,
                                    DLL_DIRECTORY_COOKIE *cookie) {
  wchar_t trusted[MAX_PATH * 4u];
  int count;
  if (!path || !cookie || *cookie) return 0;
  count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                              trusted,
                              (int)(sizeof(trusted) / sizeof(trusted[0])));
  if (count <= 0 || count >= (int)(sizeof(trusted) / sizeof(trusted[0])))
    return 0;
  *cookie = AddDllDirectory(trusted);
  return *cookie != NULL;
}
static int flowie_test_register_sdk_runtime(void) {
  /* Both locations originate from the immutable published SDK graph;
   * nothing is searched through PATH, the working directory or runner. */
  if (!flowie_test_trust_sdk_bin(FLOWIE_TEST_TURBODB_RUNTIME_DIR,
                                 &flowie_sdk_turbodb_cookie)) return 0;
  if (!flowie_test_trust_sdk_bin(FLOWIE_TEST_SALTS_RUNTIME_DIR,
                                 &flowie_sdk_salts_cookie)) return 0;
  return 1;
}
static int flowie_test_release_sdk_runtime(void) {
  int status = 1;
  if (flowie_sdk_salts_cookie) {
    if (!RemoveDllDirectory(flowie_sdk_salts_cookie)) status = 0;
    else flowie_sdk_salts_cookie = NULL;
  }
  if (flowie_sdk_turbodb_cookie) {
    if (!RemoveDllDirectory(flowie_sdk_turbodb_cookie)) status = 0;
    else flowie_sdk_turbodb_cookie = NULL;
  }
  return status;
}
#endif

spec("Flowie TurboDB driver runtime") {
  static orm_config_t config;
  static orm_option_t options[FLOWIE_ORM_MAX_OPTIONS + 1u];
  static orm_connection_t *connection;
  static orm_connection_t *second;
  static orm_query_t *query;
  static orm_result_t *result;
  static orm_error_t error;

  before_each() {
#if defined(_WIN32)
    check_true(flowie_test_register_sdk_runtime());
#endif
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
#if defined(_WIN32)
    /* Scope the trust grant to one test and AFTER all plugin/connection
     * handles have released their module leases. */
    check_true(flowie_test_release_sdk_runtime());
#endif
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

  it("loads the SDK SQL drivers and the postgres alias before validating connect limits") {
    /* TidesDB is an optional producer component, absent from the Windows SDK. */
    const char *drivers[] = {"sqlite", "postgresql", "postgres", "mysql"};
    /* Invalid generic limits fail before a backend can contact a database. */
    config.max_parameters = 0u;
    for (size_t index = 0u; index < sizeof(drivers) / sizeof(drivers[0]); ++index) {
      orm_status_t status;
      config.driver = orm_view(drivers[index]);
      status = flowie_orm_connect(&config, &connection, &error);
      info("driver=%s: %s", drivers[index], error.message);
      check_equal(status, ORM_STATUS_INVALID_ARGUMENT);
      check_null(connection);
      check_contains(error.message, "configuration limits");
    }
  }

  it("reports the retired TidesDB driver as a missing module") {
    orm_status_t status;
    config.driver = orm_view("tidesdb");
    status = flowie_orm_connect(&config, &connection, &error);
    check_true(status == ORM_STATUS_DRIVER_MODULE_NOT_FOUND || status == ORM_STATUS_DRIVER_LOAD_ERROR);
    check_null(connection);
    check_equal(error.status, status);
    check_not_equal(error.message[0], '\0');
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
