#include "flowie_control_runtime_internal.h"
#include "flowie_control_store_internal.h"
#include "flowie_control_test_turbodb.h"

#include "cmeta_error.h"
#include "tinytest.h"
#include "tls_test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

spec("Flowie controller bootstrap runtime") {
  it("rolls back identity and application assembly while retaining idempotent bootstrap commits") {
    char cert_file[512] = {0};
    char key_file[512] = {0};
    char *database_path = tt_make_temp_file("flowie-control-assembly-rollback", ".sqlite3");
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    flowie_control_runtime_t *runtime = NULL;
    flowie_control_store_config_t store_config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
    flowie_control_test_turbodb_t test_database;
    flowie_control_store_t *store = NULL;
    uint64_t revision_after_failure = 0u;
    uint64_t revision_after_retry = 0u;
    int header_count = config.listener.limits.max_headers_count;
    size_t session_capacity = config.management.session_capacity;

    check_not_null(database_path);
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    (void)snprintf(config.listener.tls.cert_file, sizeof(config.listener.tls.cert_file), "%s",
                   cert_file);
    (void)snprintf(config.listener.tls.key_file, sizeof(config.listener.tls.key_file), "%s",
                   key_file);
    check_equal(flowie_control_test_runtime_turbodb(&config, database_path), 0);
    (void)snprintf(config.management.rpc_path, sizeof(config.management.rpc_path), "%s",
                   "/v2/control/rpc");
    /* Native App configuration fails after Repository and Identity are ready. */
    config.listener.limits.max_headers_count = 0;
    check_equal(flowie_control_runtime_validate(&config), SALTS_OK);
    check_equal(flowie_control_runtime_create(&config, &runtime), SALTS_EINVAL);
    check_null(runtime);
    check_equal(flowie_control_test_turbodb_init(&test_database, database_path), 0);
    store_config.database = &test_database.config;
    check_equal(flowie_control_store_open(&store_config, &store), SALTS_OK);
    check_equal(flowie_control_store_current_revision(store, &revision_after_failure), SALTS_OK);
    check_true(revision_after_failure != 0u);
    flowie_control_store_destroy(store);
    store = NULL;

    config.listener.limits.max_headers_count = header_count;
    /* Identity fails after creating its auth service; Repository must also release. */
    config.management.session_capacity = 0u;
    check_equal(flowie_control_runtime_create(&config, &runtime), SALTS_EINVAL);
    check_null(runtime);
    check_equal(flowie_control_store_open(&store_config, &store), SALTS_OK);
    check_equal(flowie_control_store_current_revision(store, &revision_after_retry), SALTS_OK);
    check_equal(revision_after_retry, revision_after_failure);
    flowie_control_store_destroy(store);
    store = NULL;

    config.management.session_capacity = session_capacity;
    check_equal(flowie_control_runtime_create(&config, &runtime), SALTS_OK);
    check_equal(flowie_control_runtime_destroy(runtime), SALTS_OK);
    runtime = NULL;
    check_equal(flowie_control_store_open(&store_config, &store), SALTS_OK);
    check_equal(flowie_control_store_current_revision(store, &revision_after_retry), SALTS_OK);
    check_equal(revision_after_retry, revision_after_failure);
    flowie_control_store_destroy(store);
    check_equal(tt_remove_file(database_path), 0);
    free(database_path);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
  }

  it("initializes an empty store with the fixed system administrator credential") {
    char cert_file[512] = {0};
    char key_file[512] = {0};
    char *database_path = tt_make_temp_file("flowie-control-runtime-bootstrap", ".sqlite3");
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    flowie_control_runtime_t *runtime = NULL;
    flowie_control_store_config_t store_config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
    flowie_control_test_turbodb_t test_database;
    flowie_control_store_t *store = NULL;
    flowie_control_credential_verify_result_t credential =
        FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;

    check_not_null(database_path);
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    (void)snprintf(config.management.rpc_path, sizeof(config.management.rpc_path), "%s",
                   "/v2/control/rpc");
    config.dashboard_enabled = 0;
    (void)snprintf(config.listener.tls.cert_file, sizeof(config.listener.tls.cert_file), "%s",
                   cert_file);
    (void)snprintf(config.listener.tls.key_file, sizeof(config.listener.tls.key_file), "%s",
                   key_file);
    check_equal(flowie_control_test_runtime_turbodb(&config, database_path), 0);

    check_equal(flowie_control_runtime_create(&config, &runtime), SALTS_OK);
    check_not_null(runtime);
    check_equal(flowie_control_runtime_destroy(runtime), SALTS_OK);
    check_equal(flowie_control_test_turbodb_init(&test_database, database_path), 0);
    store_config.database = &test_database.config;
    check_equal(flowie_control_store_open(&store_config, &store), SALTS_OK);
    check_equal(flowie_control_store_repository(store)->auth->credential_verify(
                    flowie_control_store_repository(store)->ctx, "system", "admin",
                    FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD,
                    sizeof(FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD) - 1u, &credential),
                SALTS_OK);
    flowie_control_store_destroy(store);
    check_equal(tt_remove_file(database_path), 0);
    free(database_path);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
  }
}
