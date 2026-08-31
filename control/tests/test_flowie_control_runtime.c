#include "flowie_control_bootstrap_internal.h"
#include "flowie_control_runtime_internal.h"
#include "flowie_control_test_turbodb.h"

#include "flowie_test_socket.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include "turbo_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FINGERPRINT "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static const flowie_control_config_t RUNTIME_TEST_DEFAULT_CONFIG = FLOWIE_CONTROL_CONFIG_INIT;

typedef struct control_runtime_fixture_s {
  char *path;
  flowie_control_store_t *store;
  uint64_t revision;
} control_runtime_fixture_t;

static int runtime_test_set_env(const char *name, const char *value) {
#ifdef _WIN32
  return _putenv_s(name, value ? value : "");
#else
  return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

static int runtime_domain_create(flowie_control_store_t *store, uint64_t revision) {
  flowie_control_domain_create_command_t command = FLOWIE_CONTROL_DOMAIN_CREATE_COMMAND_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  command.domain_id = "root-a";
  command.actor = "bootstrap";
  command.request_id = "root-create";
  command.expected_revision = revision;
  command.occurred_at = 1000u;
  return flowie_control_store_domain_create(store, &command, &result);
}

static int runtime_user_create(flowie_control_store_t *store, uint64_t revision) {
  flowie_control_user_create_command_t command = FLOWIE_CONTROL_USER_CREATE_COMMAND_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  command.domain_id = "root-a";
  command.principal_id = "admin-a";
  command.principal_type = "operator";
  command.actor = "bootstrap";
  command.request_id = "user-create";
  command.expected_revision = revision;
  command.occurred_at = 1001u;
  return flowie_control_store_user_create(store, &command, &result);
}

static int runtime_role_create(flowie_control_store_t *store, const char *role,
                               const char *request_id, uint64_t revision) {
  flowie_control_role_create_command_t command = FLOWIE_CONTROL_ROLE_CREATE_COMMAND_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  command.domain_id = "root-a";
  command.role_id = role;
  command.actor = "bootstrap";
  command.request_id = request_id;
  command.expected_revision = revision;
  command.occurred_at = 1000u + revision;
  return flowie_control_store_role_create(store, &command, &result);
}

static int runtime_role_add(flowie_control_store_t *store, const char *role, const char *request_id,
                            uint64_t revision) {
  flowie_control_user_role_add_command_t command = FLOWIE_CONTROL_USER_ROLE_ADD_COMMAND_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  command.domain_id = "root-a";
  command.principal_id = "admin-a";
  command.role_id = role;
  command.actor = "bootstrap";
  command.request_id = request_id;
  command.expected_revision = revision;
  command.occurred_at = 1000u + revision;
  return flowie_control_store_user_role_add(store, &command, &result);
}

static control_runtime_fixture_t runtime_fixture_open(void) {
  control_runtime_fixture_t fixture = {0};
  flowie_control_store_config_t config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
  flowie_control_test_turbodb_t test_database;
  fixture.path = tt_make_temp_file("flowie-control-runtime", ".sqlite3");
  check_not_null(fixture.path);
  check_equal(flowie_control_test_turbodb_init(&test_database, fixture.path), 0);
  config.database = &test_database.config;
  check_equal(flowie_control_store_open(&config, &fixture.store), TURBO_OK);
  {
    flowie_control_config_t runtime_config = FLOWIE_CONTROL_CONFIG_INIT;
    check_equal(flowie_control_bootstrap_apply(
                    flowie_control_store_repository(fixture.store), &runtime_config.bootstrap,
                    FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD,
                    sizeof(FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD) - 1u, 900u),
                TURBO_OK);
  }
  check_equal(flowie_control_store_current_revision(fixture.store, &fixture.revision), TURBO_OK);
  check_equal(runtime_domain_create(fixture.store, fixture.revision), TURBO_OK);
  check_equal(flowie_control_store_current_revision(fixture.store, &fixture.revision), TURBO_OK);
  check_equal(runtime_user_create(fixture.store, fixture.revision), TURBO_OK);
  check_equal(flowie_control_store_current_revision(fixture.store, &fixture.revision), TURBO_OK);
  return fixture;
}

static void runtime_fixture_close(control_runtime_fixture_t *fixture) {
  flowie_control_store_destroy(fixture->store);
  check_equal(tt_remove_file(fixture->path), 0);
  free(fixture->path);
  memset(fixture, 0, sizeof(*fixture));
}

static void runtime_jwt_jwks_composition_test(void) {
  char cert_file[512] = {0};
  char key_file[512] = {0};
  control_runtime_fixture_t fixture = runtime_fixture_open();
  flowie_control_config_t *config = (flowie_control_config_t *)malloc(sizeof(*config));
  flowie_control_runtime_t *runtime = NULL;

  check_not_null(config);
  memcpy(config, &RUNTIME_TEST_DEFAULT_CONFIG, sizeof(*config));
  flowie_control_store_destroy(fixture.store);
  fixture.store = NULL;
  check_equal(tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)),
              0);
  check_equal(flowie_control_test_runtime_turbodb(config, fixture.path), 0);
  (void)snprintf(config->management.rpc_path, sizeof(config->management.rpc_path), "%s",
                 "/v2/control/rpc");
  (void)snprintf(config->listener.tls.cert_file, sizeof(config->listener.tls.cert_file), "%s",
                 cert_file);
  (void)snprintf(config->listener.tls.key_file, sizeof(config->listener.tls.key_file), "%s",
                 key_file);
  config->auth.enabled = 1;
  (void)snprintf(config->auth.listener_id, sizeof(config->auth.listener_id), "%s",
                 "flowie-control-auth");
  (void)snprintf(config->auth.method, sizeof(config->auth.method), "%s", "bearer");
  config->auth.jwt_jwks.enabled = 1;
  (void)snprintf(config->auth.jwt_jwks.url, sizeof(config->auth.jwt_jwks.url), "%s",
                 "https://identity.example/.well-known/jwks.json");
  (void)snprintf(config->auth.jwt_jwks.trusted_issuer, sizeof(config->auth.jwt_jwks.trusted_issuer),
                 "%s", "https://identity.example");
  (void)snprintf(config->auth.jwt_jwks.audience, sizeof(config->auth.jwt_jwks.audience), "%s",
                 "flowie");
  (void)snprintf(config->auth.jwt_jwks.subject_type, sizeof(config->auth.jwt_jwks.subject_type),
                 "%s", "device");
  (void)snprintf(config->auth.jwt_jwks.algorithm, sizeof(config->auth.jwt_jwks.algorithm), "%s",
                 "EdDSA");
  (void)snprintf(config->auth.jwt_jwks.ca_file, sizeof(config->auth.jwt_jwks.ca_file), "%s",
                 cert_file);

  check_equal(flowie_control_runtime_create(config, &runtime), TURBO_OK);
  check_not_null(runtime);
  check_equal(flowie_control_runtime_destroy(runtime), TURBO_OK);

  free(config);
  tls_test_remove_file(key_file);
  tls_test_remove_file(cert_file);
  runtime_fixture_close(&fixture);
}

spec("Flowie controller runtime") {
  it("resolves a logged-in principal to current reserved roles") {
    control_runtime_fixture_t fixture = runtime_fixture_open();
    flowie_control_management_caller_t caller = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;

    check_equal(flowie_control_management_identity_resolve_principal(
                    flowie_control_store_repository(fixture.store), "root-a", "admin-a", &caller),
                TURBO_EPERM);
    check_equal(runtime_role_create(fixture.store, FLOWIE_CONTROL_MANAGEMENT_ROLE_VIEWER,
                                    "role-viewer", fixture.revision),
                TURBO_OK);
    check_equal(runtime_role_add(fixture.store, FLOWIE_CONTROL_MANAGEMENT_ROLE_VIEWER,
                                 "assign-viewer", fixture.revision + 1u),
                TURBO_OK);
    check_equal(runtime_role_create(fixture.store, FLOWIE_CONTROL_MANAGEMENT_ROLE_USER_ADMIN,
                                    "role-user-admin", fixture.revision + 2u),
                TURBO_OK);
    check_equal(runtime_role_add(fixture.store, FLOWIE_CONTROL_MANAGEMENT_ROLE_USER_ADMIN,
                                 "assign-user-admin", fixture.revision + 3u),
                TURBO_OK);
    check_equal(flowie_control_management_identity_resolve_principal(
                    flowie_control_store_repository(fixture.store), "root-a", "admin-a", &caller),
                TURBO_OK);
    check_equal(caller.domain_id, "root-a");
    check_equal(caller.actor, "admin-a");
    check_equal(caller.permissions,
                FLOWIE_CONTROL_MANAGEMENT_VIEWER | FLOWIE_CONTROL_MANAGEMENT_USER_ADMIN);
    runtime_fixture_close(&fixture);
  }

  it("rejects principals that do not exist in the presented Domain") {
    control_runtime_fixture_t fixture = runtime_fixture_open();
    flowie_control_management_caller_t caller = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;

    check_equal(
        flowie_control_management_identity_resolve_principal(
            flowie_control_store_repository(fixture.store), "root-a", "missing-admin", &caller),
        TURBO_EPERM);
    check_null(caller.domain_id);
    check_null(caller.actor);
    runtime_fixture_close(&fixture);
  }

  it("fails closed when TLS identity files cannot be validated") {
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    check_equal(flowie_control_test_runtime_turbodb(&config, ":memory:"), 0);
    memcpy(config.management.rpc_path, "/v2/control/rpc", sizeof("/v2/control/rpc"));
    memcpy(config.listener.tls.cert_file, "missing-control-cert.pem",
           sizeof("missing-control-cert.pem"));
    memcpy(config.listener.tls.key_file, "missing-control-key.pem",
           sizeof("missing-control-key.pem"));
    memcpy(config.listener.tls.client_ca_file, "missing-control-ca.pem",
           sizeof("missing-control-ca.pem"));

    check_equal(flowie_control_runtime_validate(&config), TURBO_EIO);
  }

  it("rejects RPC routes that collide with fixed Dashboard routes") {
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    memcpy(config.management.rpc_path, "/v2/control/dashboard", sizeof("/v2/control/dashboard"));

    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);
  }

  it("rejects incomplete local auth configuration") {
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    memcpy(config.management.rpc_path, "/v2/control/rpc", sizeof("/v2/control/rpc"));
    config.auth.enabled = 1;

    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);
  }

  it("rejects invalid local executor bounds before TLS startup") {
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    memcpy(config.management.rpc_path, "/v2/control/rpc", sizeof("/v2/control/rpc"));
    config.auth.enabled = 1;
    config.auth.local_executor.workers = 0u;

    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);
  }

  it("rejects simultaneous local executor and external HTTPS modes") {
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    memcpy(config.management.rpc_path, "/v2/control/rpc", sizeof("/v2/control/rpc"));
    config.auth.enabled = 1;
    config.auth.local_executor.configured = 1;
    config.auth.external_https.enabled = 1;

    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);
  }

  it("requires a configured TurboDB driver") {
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    (void)snprintf(config.management.rpc_path, sizeof(config.management.rpc_path), "%s",
                   "/v2/control/rpc");
    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);
  }

  it("rejects programmatic plaintext and malformed TurboDB secret options") {
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    check_equal(flowie_control_test_runtime_turbodb(&config, ":memory:"), 0);
    (void)snprintf(config.management.rpc_path, sizeof(config.management.rpc_path), "%s",
                   "/v2/control/rpc");
    (void)snprintf(config.turbodb.options[0].keyword, sizeof(config.turbodb.options[0].keyword),
                   "%s", "conninfo");
    (void)snprintf(config.turbodb.options[0].value, sizeof(config.turbodb.options[0].value), "%s",
                   "host=db.example password=literal");
    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);

    (void)snprintf(config.turbodb.options[0].keyword, sizeof(config.turbodb.options[0].keyword),
                   "%s", "sslpassword");
    (void)snprintf(config.turbodb.options[0].value, sizeof(config.turbodb.options[0].value), "%s",
                   "env://BAD-NAME");
    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);
  }

  it("validates the configured external HTTPS identity and secrets before startup") {
    char cert_file[512] = {0};
    char key_file[512] = {0};
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;

    check_equal(flowie_control_test_runtime_turbodb(&config, ":memory:"), 0);
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    check_equal(runtime_test_set_env("FLOWIE_RUNTIME_EXTERNAL_TOKEN", "service-token"), 0);
    (void)snprintf(config.management.rpc_path, sizeof(config.management.rpc_path), "%s",
                   "/v2/control/rpc");
    (void)snprintf(config.listener.tls.cert_file, sizeof(config.listener.tls.cert_file), "%s",
                   cert_file);
    (void)snprintf(config.listener.tls.key_file, sizeof(config.listener.tls.key_file), "%s",
                   key_file);
    (void)snprintf(config.listener.tls.client_ca_file, sizeof(config.listener.tls.client_ca_file),
                   "%s", cert_file);
    config.auth.external_https.enabled = 1;
    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);
    config.auth.enabled = 1;
    (void)snprintf(config.auth.method, sizeof(config.auth.method), "%s", "bearer");
    (void)snprintf(config.auth.external_https.url, sizeof(config.auth.external_https.url), "%s",
                   "https://localhost/v1/assert");
    (void)snprintf(config.auth.external_https.service_token_ref,
                   sizeof(config.auth.external_https.service_token_ref), "%s",
                   "env://FLOWIE_RUNTIME_EXTERNAL_TOKEN");
    (void)snprintf(config.auth.external_https.trusted_issuer,
                   sizeof(config.auth.external_https.trusted_issuer), "%s",
                   "https://identity.example");
    (void)snprintf(config.auth.external_https.subject_type,
                   sizeof(config.auth.external_https.subject_type), "%s", "device");
    (void)snprintf(config.auth.external_https.tls.ca_file,
                   sizeof(config.auth.external_https.tls.ca_file), "%s", cert_file);
    (void)snprintf(config.auth.external_https.tls.client_cert_file,
                   sizeof(config.auth.external_https.tls.client_cert_file), "%s", cert_file);
    (void)snprintf(config.auth.external_https.tls.client_key_file,
                   sizeof(config.auth.external_https.tls.client_key_file), "%s", key_file);

    check_equal(flowie_control_runtime_validate(&config), TURBO_OK);
    (void)snprintf(config.auth.external_https.tls.ca_file,
                   sizeof(config.auth.external_https.tls.ca_file), "%s", "missing-external-ca.pem");
    check_equal(flowie_control_runtime_validate(&config), TURBO_EIO);

    check_equal(runtime_test_set_env("FLOWIE_RUNTIME_EXTERNAL_TOKEN", NULL), 0);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
  }

  it("validates JWT JWKS trust and bounded executor configuration before startup") {
    char cert_file[512] = {0};
    char key_file[512] = {0};
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;

    check_equal(flowie_control_test_runtime_turbodb(&config, ":memory:"), 0);
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    (void)snprintf(config.management.rpc_path, sizeof(config.management.rpc_path), "%s",
                   "/v2/control/rpc");
    (void)snprintf(config.listener.tls.cert_file, sizeof(config.listener.tls.cert_file), "%s",
                   cert_file);
    (void)snprintf(config.listener.tls.key_file, sizeof(config.listener.tls.key_file), "%s",
                   key_file);
    config.auth.enabled = 1;
    (void)snprintf(config.auth.method, sizeof(config.auth.method), "%s", "bearer");
    config.auth.jwt_jwks.enabled = 1;
    (void)snprintf(config.auth.jwt_jwks.url, sizeof(config.auth.jwt_jwks.url), "%s",
                   "https://identity.example/.well-known/jwks.json");
    (void)snprintf(config.auth.jwt_jwks.trusted_issuer, sizeof(config.auth.jwt_jwks.trusted_issuer),
                   "%s", "https://identity.example");
    (void)snprintf(config.auth.jwt_jwks.audience, sizeof(config.auth.jwt_jwks.audience), "%s",
                   "flowie");
    (void)snprintf(config.auth.jwt_jwks.subject_type, sizeof(config.auth.jwt_jwks.subject_type),
                   "%s", "device");
    (void)snprintf(config.auth.jwt_jwks.algorithm, sizeof(config.auth.jwt_jwks.algorithm), "%s",
                   "EdDSA");
    (void)snprintf(config.auth.jwt_jwks.ca_file, sizeof(config.auth.jwt_jwks.ca_file), "%s",
                   cert_file);

    check_equal(flowie_control_runtime_validate(&config), TURBO_OK);
    config.auth.jwt_jwks.executor_workers = 0u;
    check_equal(flowie_control_runtime_validate(&config), TURBO_EINVAL);
    config.auth.jwt_jwks.executor_workers =
        FLOWIE_CONTROL_CONFIG_AUTH_LOCAL_EXECUTOR_DEFAULT_WORKERS;
    (void)snprintf(config.auth.jwt_jwks.ca_file, sizeof(config.auth.jwt_jwks.ca_file), "%s",
                   "missing-jwks-ca.pem");
    check_equal(flowie_control_runtime_validate(&config), TURBO_EIO);

    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
  }

  it("composes JWT JWKS auth for management and broker endpoints") {
    runtime_jwt_jwks_composition_test();
  }

  it("composes external HTTPS auth for management and broker endpoints") {
    char cert_file[512] = {0};
    char key_file[512] = {0};
    control_runtime_fixture_t fixture = runtime_fixture_open();
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    flowie_control_runtime_t *runtime = NULL;

    check_equal(runtime_role_create(fixture.store, FLOWIE_CONTROL_MANAGEMENT_ROLE_VIEWER,
                                    "runtime-viewer", fixture.revision),
                TURBO_OK);
    check_equal(runtime_role_add(fixture.store, FLOWIE_CONTROL_MANAGEMENT_ROLE_VIEWER,
                                 "runtime-viewer-add", fixture.revision + 1u),
                TURBO_OK);
    flowie_control_store_destroy(fixture.store);
    fixture.store = NULL;
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    check_equal(runtime_test_set_env("FLOWIE_RUNTIME_EXTERNAL_TOKEN", "outbound-token"), 0);
    (void)snprintf(config.management.rpc_path, sizeof(config.management.rpc_path), "%s",
                   "/v2/control/rpc");
    config.dashboard_enabled = 1;
    (void)snprintf(config.listener.tls.cert_file, sizeof(config.listener.tls.cert_file), "%s",
                   cert_file);
    (void)snprintf(config.listener.tls.key_file, sizeof(config.listener.tls.key_file), "%s",
                   key_file);
    check_equal(flowie_control_test_runtime_turbodb(&config, fixture.path), 0);
    config.auth.enabled = 1;
    (void)snprintf(config.auth.listener_id, sizeof(config.auth.listener_id), "%s",
                   "flowie-control-auth");
    (void)snprintf(config.auth.method, sizeof(config.auth.method), "%s", "bearer");
    config.auth.external_https.enabled = 1;
    (void)snprintf(config.auth.external_https.url, sizeof(config.auth.external_https.url), "%s",
                   "https://localhost/v1/assert");
    (void)snprintf(config.auth.external_https.service_token_ref,
                   sizeof(config.auth.external_https.service_token_ref), "%s",
                   "env://FLOWIE_RUNTIME_EXTERNAL_TOKEN");
    (void)snprintf(config.auth.external_https.trusted_issuer,
                   sizeof(config.auth.external_https.trusted_issuer), "%s",
                   "https://identity.example");
    (void)snprintf(config.auth.external_https.subject_type,
                   sizeof(config.auth.external_https.subject_type), "%s", "device");
    (void)snprintf(config.auth.external_https.tls.ca_file,
                   sizeof(config.auth.external_https.tls.ca_file), "%s", cert_file);
    (void)snprintf(config.auth.external_https.tls.client_cert_file,
                   sizeof(config.auth.external_https.tls.client_cert_file), "%s", cert_file);
    (void)snprintf(config.auth.external_https.tls.client_key_file,
                   sizeof(config.auth.external_https.tls.client_key_file), "%s", key_file);

    check_equal(flowie_control_runtime_create(&config, &runtime), TURBO_OK);
    check_not_null(runtime);
    check_equal(flowie_control_runtime_destroy(runtime), TURBO_OK);
    runtime = NULL;

    check_equal(runtime_test_set_env("FLOWIE_RUNTIME_EXTERNAL_TOKEN", NULL), 0);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
    runtime_fixture_close(&fixture);
  }

  it("starts and stops an owned HTTPS listener without process signal handlers") {
    char cert_file[512] = {0};
    char key_file[512] = {0};
    control_runtime_fixture_t fixture = runtime_fixture_open();
    flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
    flowie_control_runtime_t *runtime = NULL;

    flowie_control_store_destroy(fixture.store);
    fixture.store = NULL;
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    (void)snprintf(config.listener.host, sizeof(config.listener.host), "%s", "127.0.0.1");
    config.listener.port = flowie_test_port();
    check_true(config.listener.port != 0u);
    (void)snprintf(config.listener.tls.cert_file, sizeof(config.listener.tls.cert_file), "%s",
                   cert_file);
    (void)snprintf(config.listener.tls.key_file, sizeof(config.listener.tls.key_file), "%s",
                   key_file);
    (void)snprintf(config.management.rpc_path, sizeof(config.management.rpc_path), "%s",
                   "/v2/control/rpc");
    check_equal(flowie_control_test_runtime_turbodb(&config, fixture.path), 0);

    check_equal(flowie_control_runtime_create(&config, &runtime), TURBO_OK);
    check_not_null(runtime);
    check_equal(flowie_control_runtime_start(runtime), TURBO_OK);
    check_equal(flowie_control_runtime_start(runtime), TURBO_EINVAL);
    check_equal(flowie_control_runtime_stop(runtime), TURBO_OK);
    check_equal(flowie_control_runtime_stop(runtime), TURBO_OK);
    check_equal(flowie_control_runtime_destroy(runtime), TURBO_OK);

    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
    runtime_fixture_close(&fixture);
  }
}
