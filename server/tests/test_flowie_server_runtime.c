#include "flowie_server_runtime_internal.h"
#include "flowie_server_turbodb_config_internal.h"
#include "tinytest.h"
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
  static flowie_endpoint_config_t endpoint;
  static flowie_endpoint_core_options_t options;
  static const char *operation;

  before_each() {
    runtime = NULL;
    database = NULL;
    operation = NULL;
    endpoint = (flowie_endpoint_config_t)FLOWIE_ENDPOINT_CONFIG_INIT;
    endpoint.host = "127.0.0.1";
    endpoint.port = 1883;
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
    flowie_server_turbodb_config_destroy(database);
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
