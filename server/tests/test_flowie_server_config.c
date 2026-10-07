#include "flowie_server_config_internal.h"

#include "flowie.h"

#include "tinytest.h"
#include "cmeta_error.h"

#include <string.h>

spec("Flowie standalone YAML configuration") {
  it("loads network owner count and aggregate mailbox budget") {
    flowie_server_config_t *config = NULL;
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    const flowie_endpoint_config_t *endpoint;
    check_equal(flowie_server_config_load(FLOWIE_TEST_MULTI_CONFIG, "multicore", 0,
                                           &config, &error), SALTS_OK);
    endpoint = flowie_server_config_endpoint(config);
    check_equal(endpoint->network_workers, 2u);
    check_equal(endpoint->max_connections, 3u);
    check_equal(endpoint->network_command_bytes, 8192u);
    check_equal(endpoint->network_policy, FLOWIE_NETWORK_LEAST_CONNECTIONS);
    check_equal(endpoint->network_cpu_count, 2u);
    check_equal(endpoint->network_cpus[0], 2u);
    check_equal(endpoint->network_cpus[1], 4u);
    flowie_server_config_destroy(config);
  }

  it("rejects network partitions larger than capacity and unsupported transports") {
    flowie_server_config_t *config = NULL;
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    check_equal(flowie_server_config_load(FLOWIE_TEST_MULTI_CONFIG, "too_many", 0,
                                           &config, &error), SALTS_ERANGE);
    check_null(config);
    check_equal(flowie_server_config_load(FLOWIE_TEST_MULTI_CONFIG, "websocket", 0,
                                           &config, &error), SALTS_ENOTSUP);
    check_null(config);
  }

  it("validates YAML owner placement and accepts explicit OS scheduling") {
    static const struct { const char *profile; int status; } cases[] = {
      {"bad_policy", SALTS_EINVAL}, {"bad_cpu_count", SALTS_EINVAL},
      {"bad_cpu_value", SALTS_EINVAL}, {"bad_cpu_type", SALTS_EPROTO},
      {"udp_placement", SALTS_ENOTSUP}};
    flowie_server_config_t *config = NULL;
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    const flowie_endpoint_config_t *endpoint;
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
      check_equal(flowie_server_config_load(FLOWIE_TEST_MULTI_CONFIG, cases[index].profile,
                                            0, &config, &error), cases[index].status);
      check_null(config);
    }
    check_equal(flowie_server_config_load(FLOWIE_TEST_MULTI_CONFIG, "default_placement",
                                          0, &config, &error), SALTS_OK);
    endpoint = flowie_server_config_endpoint(config);
    check_equal(endpoint->network_policy, FLOWIE_NETWORK_ROUND_ROBIN);
    check_equal(endpoint->network_cpu_count, 0u);
    flowie_server_config_destroy(config);
  }

  it("parses CLI placement strictly and preserves settings after invalid input") {
    flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
    static const char *invalid[] = {"", "-1", "+1", " 1", "1,", ",1", "1,,2", "1,a", "0x2"};
    check_equal(flowie_server_network_policy_parse("least-connections", &config), SALTS_OK);
    check_equal(config.network_policy, FLOWIE_NETWORK_LEAST_CONNECTIONS);
    check_equal(flowie_server_network_policy_parse("random", &config), SALTS_EINVAL);
    check_equal(config.network_policy, FLOWIE_NETWORK_LEAST_CONNECTIONS);
    check_equal(flowie_server_network_policy_parse("round-robin", &config), SALTS_OK);
    check_equal(config.network_policy, FLOWIE_NETWORK_ROUND_ROBIN);
    check_equal(flowie_server_network_cpus_parse("2,4", &config), SALTS_OK);
    for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
      check_equal(flowie_server_network_cpus_parse(invalid[index], &config), SALTS_EINVAL);
      check_equal(config.network_cpu_count, 2u);
      check_equal(config.network_cpus[0], 2u);
      check_equal(config.network_cpus[1], 4u);
    }
    check_equal(flowie_server_network_cpus_parse("4294967296", &config), SALTS_ERANGE);
    check_equal(config.network_cpus[0], 2u);
    check_equal(flowie_server_network_cpus_parse("0,0", &config), SALTS_OK);
    check_equal(config.network_cpus[0], 0u);
    check_equal(config.network_cpus[1], 0u);
  }

  it("loads the deployed EU endpoint and complete HTTPS security chain") {
    flowie_server_config_t *config = NULL;
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    const flowie_endpoint_config_t *endpoint;
    const flowie_server_http_provider_config_t *auth;
    const flowie_server_http_provider_config_t *acl;

    check_equal(flowie_server_config_load(FLOWIE_TEST_EU_CONFIG, "flowie", 1, &config, &error),
                SALTS_OK);
    check_not_null(config);
    endpoint = flowie_server_config_endpoint(config);
    auth = flowie_server_config_auth(config);
    acl = flowie_server_config_acl(config);
    check_not_null(endpoint);
    check_not_null(auth);
    check_not_null(acl);
    check_equal(endpoint->host, "127.0.0.1");
    check_equal(endpoint->port, 18883);
    check_equal(endpoint->manage_sessions, 1);
    check_equal(endpoint->network_policy, FLOWIE_NETWORK_ROUND_ROBIN);
    check_equal(endpoint->network_cpu_count, 0u);
    check_equal(flowie_server_config_endpoint_name(config), "mqtt.endpoint");
    check_equal(flowie_server_config_realm_name(config), "mqtt.security");
    check_equal(flowie_server_config_realm_resource_uid(config), "security:mqtt");
    check_equal(flowie_server_config_realm_owner_name(config), "security.main");
    check_equal(flowie_server_config_acl_provider_name(config), "mqtt.acl-service");
    check_equal(flowie_server_config_auth_method(config), "password");
    check_equal(auth->url, "https://127.0.0.1:8443/v4/authenticate");
    check_equal(acl->url, "https://127.0.0.1:8443/v4/acl/check");
    check_equal(auth->service_id, "broker-main");
    check_equal(acl->service_domain, "platform-services");
    check_equal(auth->service_token_ref, "env://FLOWIE_AUTH_SERVICE_TOKEN");
    check_equal(auth->ca_file, "/etc/flowie/certs/control-ca.crt");
    check_equal(acl->ca_file, "/etc/flowie/certs/control-ca.crt");
    check_equal(auth->timeout_ms, 3000u);
    check_equal(auth->max_body_size, 4096u);
    check_equal(acl->max_body_size, 65536u);
    flowie_server_config_destroy(config);
  }

  it("fails closed when security is required but the profile has no auth provider") {
    flowie_server_config_t *config = NULL;
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;

    check_equal(flowie_server_config_load(FLOWIE_TEST_INSECURE_CONFIG, "flowie", 1, &config,
                                          &error),
                SALTS_EINVAL);
    check_null(config);
    check_true(error.path[0] != '\0');
  }
}
