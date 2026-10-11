#include "flowie_server_runtime_internal.h"
#include "flowie_server_turbodb_config_internal.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include <salts/error_codes.h>

#include <stdint.h>
#include <string.h>

static int server_test_message(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
    flowie_publish_result_t *result, void *context) {
  (void)endpoint;
  (void)message;
  (void)context;
  result->status = SALTS_OK;
  result->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_ACCEPTED;
  return SALTS_OK;
}

spec("Flowie server Component Configurator") {
  static flowie_server_runtime_t *runtime;
  static flowie_server_turbodb_config_t *database;
  static flowie_server_config_t *config;
  static flowie_endpoint_config_t endpoint;
  static flowie_endpoint_core_options_t options;
  static const char *operation;
  static char cert_path[512];
  static char key_path[512];

  before_each() {
    runtime = NULL;
    database = NULL;
    config = NULL;
    cert_path[0] = key_path[0] = '\0';
    operation = NULL;
    endpoint = (flowie_endpoint_config_t)FLOWIE_ENDPOINT_CONFIG_INIT;
    endpoint.host = "127.0.0.1";
    endpoint.port = 0;
    endpoint.manage_sessions = 1;
    endpoint.max_connections = 4u;
    endpoint.max_sessions = 4u;
    endpoint.max_subscriptions_per_session = 4u;
    endpoint.max_inflight_per_session = 4u;
    endpoint.max_retained_messages = 4u;
    endpoint.max_packet_size = 4096u;
    options = (flowie_endpoint_core_options_t)FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
    options.on_message = server_test_message;
    check_equal(flowie_server_turbodb_config_create("sqlite", "{\"filename\":\":memory:\"}",
                                                    &database), SALTS_OK);
  }

  after_each() {
    check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);
    flowie_server_config_destroy(config);
    flowie_server_turbodb_config_destroy(database);
    if (cert_path[0] || key_path[0]) tls_test_clear_server_env();
    tls_test_remove_file(key_path);
    tls_test_remove_file(cert_path);
  }

  it("assembles check-only dependencies and releases the graph idempotently") {
    check_equal(flowie_server_runtime_create(NULL, &endpoint,
        flowie_server_turbodb_config_database(database), &options, 1, &runtime, &operation),
        SALTS_OK);
    check_not_null(flowie_server_runtime_endpoint(runtime));
    check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);
    check_null(runtime);
    check_null(flowie_server_runtime_endpoint(runtime));
    check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);
  }

  it("preserves repository failure and leaves no published runtime") {
    orm_config_t invalid = *flowie_server_turbodb_config_database(database);
    invalid.driver = orm_view("");
    check_equal(flowie_server_runtime_create(NULL, &endpoint, &invalid, &options,
        1, &runtime, &operation), SALTS_EINVAL);
    check_null(runtime);
    check_equal(operation, "protocol-store-open");
    check_equal(flowie_server_runtime_create(NULL, &endpoint,
        flowie_server_turbodb_config_database(database), &options, 1, &runtime, NULL), SALTS_OK);
  }

  it("starts and stops YAML WebSocket placement through the component graph") {
    const char *profiles[] = {"websocket", "secure_websocket"};
    const flowie_network_policy_t policies[] = {
        FLOWIE_NETWORK_LEAST_CONNECTIONS, FLOWIE_NETWORK_ROUND_ROBIN};
    check_equal(tls_test_write_server_files(cert_path, sizeof(cert_path),
                                            key_path, sizeof(key_path)), 0);
    check_equal(tls_test_set_server_env(cert_path, key_path), 0);
    for (size_t index = 0u; index < sizeof(profiles) / sizeof(profiles[0]); ++index) {
      flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
      check_equal(flowie_server_config_load(FLOWIE_TEST_MULTI_CONFIG, profiles[index], 0,
                                            &config, &error), SALTS_OK);
      endpoint = *flowie_server_config_endpoint(config);
      endpoint.port = 0; /* Let the OS reserve the listener without a probe/rebind race. */
      check_equal(endpoint.network_workers, 2u);
      check_equal(endpoint.network_policy, policies[index]);
      check_equal(flowie_server_runtime_create(config, &endpoint,
          flowie_server_turbodb_config_database(database), &options, 0, &runtime, &operation),
          SALTS_OK);
      check_not_null(flowie_server_runtime_endpoint(runtime));
      check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);
      check_null(runtime);
      flowie_server_config_destroy(config);
      config = NULL;
    }
  }

  it("rolls back WebSocket CPU rejection and can activate the corrected graph") {
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    check_equal(flowie_server_config_load(FLOWIE_TEST_MULTI_CONFIG, "websocket", 0,
                                          &config, &error), SALTS_OK);
    endpoint = *flowie_server_config_endpoint(config);
    endpoint.port = 0;
    endpoint.network_cpu_count = 2u;
    endpoint.network_cpus[0] = 0u;
    endpoint.network_cpus[1] = 1u;
    check_equal(flowie_server_runtime_create(config, &endpoint,
        flowie_server_turbodb_config_database(database), &options, 0, &runtime, &operation),
        SALTS_ENOTSUP);
    check_null(runtime);
    check_equal(operation, "endpoint-create");
    endpoint.network_cpu_count = 0u;
    check_equal(flowie_server_runtime_create(config, &endpoint,
        flowie_server_turbodb_config_database(database), &options, 0, &runtime, &operation),
        SALTS_OK);
    check_not_null(flowie_server_runtime_endpoint(runtime));
  }

  it("rolls back repository dependencies when endpoint creation rejects placement") {
    endpoint.network_policy = (flowie_network_policy_t)2;
    check_equal(flowie_server_runtime_create(NULL, &endpoint,
        flowie_server_turbodb_config_database(database), &options, 1, &runtime, &operation),
        SALTS_EINVAL);
    check_null(runtime);
    check_equal(operation, "endpoint-create");
    endpoint.network_policy = FLOWIE_NETWORK_ROUND_ROBIN;
    check_equal(flowie_server_runtime_create(NULL, &endpoint,
        flowie_server_turbodb_config_database(database), &options, 1, &runtime, NULL), SALTS_OK);
  }

#if defined(_WIN32) || defined(__linux__)
  it("destroys partial activation and preserves the real CPU binding error") {
    endpoint.network_cpu_count = 1u;
    endpoint.network_cpus[0] = UINT32_MAX;
    check_equal(flowie_server_runtime_create(NULL, &endpoint,
        flowie_server_turbodb_config_database(database), &options, 0, &runtime, &operation),
        SALTS_ERANGE);
    check_null(runtime);
    check_equal(operation, "endpoint-start");
    endpoint.network_cpu_count = 0u;
    check_equal(flowie_server_runtime_create(NULL, &endpoint,
        flowie_server_turbodb_config_database(database), &options, 0, &runtime, NULL), SALTS_OK);
    check_not_null(flowie_server_runtime_endpoint(runtime));
  }
#endif
}
