#include "flowie_server_config_internal.h"

#include "flowie.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

spec("Flowie standalone YAML configuration") {
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
