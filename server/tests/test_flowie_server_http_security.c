#include "flowie_server_http_security_internal.h"

#include "tinytest.h"
#include "salts_error.h"
#include <json_parser.h>

#include <string.h>

static int test_set_environment(const char *name, const char *value) {
#ifdef _WIN32
  return _putenv_s(name, value ? value : "");
#else
  return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

spec("Flowie standalone HTTPS security protocol") {
  it("uses the Flowie service identity headers required by Control") {
    flowie_server_http_provider_config_t config = {0};
    const char *headers[4] = {0};
    char service_id[sizeof("X-Flowie-Service-Id: broker-main")];
    char service_domain[sizeof("X-Flowie-Service-Domain: platform-services")];

    (void)strcpy(config.service_id, "broker-main");
    (void)strcpy(config.service_domain, "platform-services");
    check_equal(flowie_server_http_headers(&config, service_id, sizeof(service_id),
                                           service_domain, sizeof(service_domain), headers),
                SALTS_OK);
    check_equal(headers[0], "Content-Type: application/json");
    check_equal(headers[1], "Accept: application/json");
    check_equal(headers[2], "X-Flowie-Service-Id: broker-main");
    check_equal(headers[3], "X-Flowie-Service-Domain: platform-services");
  }

  it("encodes Auth v3 credentials and decodes the complete principal") {
    static const uint8_t secret[] = "secret";
    static const char response[] =
        "{\"version\":3,\"authenticated\":true,\"principal\":{"
        "\"id\":\"device-a\",\"type\":\"device\",\"domain\":\"booth\","
        "\"auth_method\":\"password\",\"scope\":\"domain\","
        "\"roles\":[\"device\"],\"groups\":[\"groups/beijing\"],"
        "\"expires_at\":0,\"policy_version\":7}}";
    flowie_security_auth_request_t request = FLOWIE_SECURITY_AUTH_REQUEST_INIT;
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    json_value_t *document = NULL;
    char *body = NULL;
    size_t body_size = 0u;

    request.identity = "device-a";
    request.method = "password";
    request.secret = secret;
    request.secret_size = sizeof(secret) - 1u;
    request.protocol = "mqtt5";
    request.remote_address = "203.0.113.5:41000";
    check_equal(flowie_server_http_auth_encode(&request, &body, &body_size), SALTS_OK);
    check_not_null(body);
    document = json_parse(body, body_size);
    check_not_null(document);
    check_equal(json_number(json_object_get(document, "version")), 3.0);
    check_equal(json_string(json_object_get(document, "identity")), "device-a");
    check_equal(json_string(json_object_get(document, "secret_base64")), "c2VjcmV0");
    json_free(document);
    flowie_server_http_body_destroy(body, body_size, 1);

    check_equal(flowie_server_http_auth_decode(response, sizeof(response) - 1u, "password",
                                                &principal),
                SALTS_OK);
    check_equal(principal.principal_id, "device-a");
    check_equal(principal.domain_id, "booth");
    check_equal(principal.scope, FLOWIE_SECURITY_SCOPE_DOMAIN);
    check_equal(principal.role_count, 1u);
    check_equal(principal.roles[0], "device");
    check_equal(principal.group_count, 1u);
    check_equal(principal.groups[0], "groups/beijing");
    check_equal(principal.policy_version, 7u);
  }

  it("encodes ACL v4 MQTT context and preserves a remote deny decision") {
    static const char response[] =
        "{\"version\":4,\"allowed\":false,\"reason\":\"deny_rule\","
        "\"policy_version\":7}";
    static const uint8_t username[] = "device-a";
    static const uint8_t client_id[] = "client-a";
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    flowie_security_request_t request = FLOWIE_SECURITY_REQUEST_INIT;
    flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;
    json_value_t *document = NULL;
    char *body = NULL;
    size_t body_size = 0u;

    (void)strcpy(principal.principal_id, "device-a");
    (void)strcpy(principal.principal_type, "device");
    (void)strcpy(principal.domain_id, "booth");
    principal.scope = FLOWIE_SECURITY_SCOPE_DOMAIN;
    principal.policy_version = 7u;
    request.principal = &principal;
    request.domain_id = principal.domain_id;
    request.action = FLOWIE_SECURITY_ACTION_PUBLISH;
    request.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
    request.resource = "booth/groups/beijing/mty/devices/mty001/camera/event";
    request.username = username;
    request.username_size = sizeof(username) - 1u;
    request.client_id = client_id;
    request.client_id_size = sizeof(client_id) - 1u;
    check_equal(flowie_server_http_acl_encode(&request, &body, &body_size), SALTS_OK);
    document = json_parse(body, body_size);
    check_not_null(document);
    check_equal(json_number(json_object_get(document, "version")), 4.0);
    check_equal(json_string(json_object_get(document, "access")), "write");
    check_equal(json_string(json_object_get(document, "topic")), request.resource);
    check_equal(json_string(json_object_get(document, "username")), "device-a");
    check_equal(json_string(json_object_get(document, "client_id")), "client-a");
    json_free(document);
    flowie_server_http_body_destroy(body, body_size, 0);

    check_equal(flowie_server_http_acl_decode(response, sizeof(response) - 1u, &decision),
                SALTS_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_DENY);
    check_equal(decision.reason, FLOWIE_SECURITY_REASON_DENY_RULE);
    check_equal(decision.policy_version, 7u);
  }

  it("resolves the service token once and exposes native Flowie providers") {
    static const uint8_t secret[] = "secret";
    flowie_server_http_provider_config_t auth = {0};
    flowie_server_http_provider_config_t acl = {0};
    flowie_server_http_security_t *security = NULL;
    flowie_security_auth_request_t auth_request = FLOWIE_SECURITY_AUTH_REQUEST_INIT;
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    flowie_security_request_t acl_request = FLOWIE_SECURITY_REQUEST_INIT;
    flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;
    (void)strcpy(auth.url, "https://127.0.0.1:8443/v4/authenticate");
    (void)strcpy(auth.method, "password");
    (void)strcpy(auth.service_id, "broker-main");
    (void)strcpy(auth.service_domain, "platform-services");
    (void)strcpy(auth.service_token_ref, "env://FLOWIE_TEST_NATIVE_HTTP_TOKEN");
    (void)strcpy(auth.ca_file, "control-ca.crt");
    auth.timeout_ms = 3000u;
    auth.max_body_size = 4096u;
    acl = auth;
    (void)strcpy(acl.url, "https://127.0.0.1:8443/v4/acl/check");
    acl.method[0] = '\0';
    acl.max_body_size = 65536u;

    check_equal(test_set_environment("FLOWIE_TEST_NATIVE_HTTP_TOKEN", NULL), 0);
    check_equal(flowie_server_http_security_create(&auth, &acl, &security), SALTS_ENOENT);
    check_null(security);
    check_equal(test_set_environment("FLOWIE_TEST_NATIVE_HTTP_TOKEN", "test-only-token"), 0);
    check_equal(flowie_server_http_security_create(&auth, &acl, &security), SALTS_OK);
    check_not_null(security);
    check_not_null(flowie_server_http_security_auth_provider(security));
    check_not_null(flowie_server_http_security_acl_provider(security));
    auth_request.identity = "device-a";
    auth_request.method = "password";
    auth_request.secret = secret;
    auth_request.secret_size = sizeof(secret) - 1u;
    auth_request.protocol = "mqtt5";
    check_equal(flowie_server_http_security_auth_provider(security)->authenticate(
                    flowie_server_http_security_auth_provider(security)->ctx, &auth_request,
                    &principal),
                SALTS_EIO);
    acl_request.principal = &principal;
    acl_request.domain_id = "booth";
    acl_request.action = FLOWIE_SECURITY_ACTION_PUBLISH;
    acl_request.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
    acl_request.resource = "booth/devices/device-a/event";
    check_equal(flowie_server_http_security_acl_provider(security)->authorize(
                    flowie_server_http_security_acl_provider(security)->ctx, &acl_request, 1u,
                    &decision),
                SALTS_EIO);
    flowie_server_http_security_destroy(security);
    check_equal(test_set_environment("FLOWIE_TEST_NATIVE_HTTP_TOKEN", NULL), 0);
  }
}
