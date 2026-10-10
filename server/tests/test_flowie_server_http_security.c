#include "flowie_server_http_security_internal.h"

#include "tinytest.h"
#include "cmeta_error.h"
#include <json_parser.h>
#include "mtls_test_server.h"
#include "fmt.h"

#include <string.h>

static int test_set_environment(const char *name, const char *value) {
#ifdef _WIN32
  return _putenv_s(name, value ? value : "");
#else
  return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

static int test_http_security_configure(unsigned short port, const char *ca_file,
                                        flowie_server_http_security_t **security) {
  flowie_server_http_provider_config_t auth = {0}, acl;
  tstr url = tstr_format("https://localhost:{}/v3/auth", (unsigned int)port);
  int rc;
  if (!url) return SALTS_ENOMEM;
  if (tstr_len(url) >= sizeof(auth.url) || strlen(ca_file) >= sizeof(auth.ca_file)) {
    tstr_free(url);
    return SALTS_ERANGE;
  }
  memcpy(auth.url, url, tstr_len(url) + 1u);
  memcpy(auth.ca_file, ca_file, strlen(ca_file) + 1u);
  memcpy(auth.method, "password", sizeof("password"));
  memcpy(auth.service_id, "broker-main", sizeof("broker-main"));
  memcpy(auth.service_domain, "platform-services", sizeof("platform-services"));
  memcpy(auth.service_token_ref, "env://FLOWIE_TEST_NATIVE_HTTP_TOKEN",
         sizeof("env://FLOWIE_TEST_NATIVE_HTTP_TOKEN"));
  auth.timeout_ms = 1000u;
  auth.max_body_size = 4096u;
  acl = auth;
  acl.method[0] = '\0';
  rc = flowie_server_http_security_create(&auth, &acl, security);
  tstr_free(url);
  return rc;
}

static flowie_security_auth_request_t test_http_auth_request(void) {
  static const uint8_t secret[] = "test-only-password";
  flowie_security_auth_request_t request = FLOWIE_SECURITY_AUTH_REQUEST_INIT;
  request.identity = "device-a";
  request.method = "password";
  request.secret = secret;
  request.secret_size = sizeof(secret) - 1u;
  request.protocol = "mqtt5";
  return request;
}

spec("Flowie server HTTPS Interceptor") {
  static flow_mtls_test_server_t server;
  static flowie_server_http_security_t *security;
  static tstr wire_response;
  static char ca_file[1025];

  before_each() {
    server = (flow_mtls_test_server_t){.listener = FLOW_MTLS_TEST_INVALID_SOCKET};
    security = NULL;
    wire_response = NULL;
    ca_file[0] = '\0';
    check_equal(test_set_environment("FLOWIE_TEST_NATIVE_HTTP_TOKEN", "test-only-token"), 0);
    check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
  }

  after_each() {
    flowie_server_http_security_destroy(security);
    flow_mtls_test_server_join(&server);
    tstr_free(wire_response);
    if (ca_file[0]) tls_test_remove_file(ca_file);
    check_equal(test_set_environment("FLOWIE_TEST_NATIVE_HTTP_TOKEN", NULL), 0);
  }

  it("unwinds successful authentication and keeps the caller request borrowed") {
    static const char body[] =
        "{\"version\":3,\"authenticated\":true,\"principal\":{\"id\":\"device-a\","
        "\"type\":\"device\",\"auth_method\":\"password\",\"domain\":\"booth\","
        "\"scope\":\"self\",\"roles\":[\"device\"],"
        "\"groups\":[],\"expires_at\":1900000000,\"policy_version\":42}}";
    flowie_security_auth_request_t request = test_http_auth_request();
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    const flowie_security_auth_provider_t *provider;
    wire_response = tstr_format("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: {}\r\nConnection: close\r\n\r\n{}", sizeof(body) - 1u, body);
    check_not_null(wire_response);
    check_equal(flow_tls_test_server_start(&server, (const uint8_t *)wire_response,
                                          tstr_len(wire_response)), 0);
    check_equal(test_http_security_configure(server.port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_auth_provider(security);
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_OK);
    flow_mtls_test_server_join(&server);
    check_equal(server.status, 0);
    check_equal(principal.principal_id, "device-a");
    check_equal(principal.policy_version, 42u);
    check_equal(memcmp(request.secret, "test-only-password", request.secret_size), 0);
    check_contains((const char *)server.request, "Authorization: Bearer test-only-token");
  }

  it("rejects invalid admission without dispatching the transport target") {
    flowie_security_auth_request_t request = test_http_auth_request();
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    const flowie_security_auth_provider_t *provider;
    /* No listener: admission errors must survive instead of becoming EIO. */
    check_equal(test_http_security_configure(1u, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_auth_provider(security);
    request.method = NULL;
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_EPERM);
    request = test_http_auth_request();
    request.size = 0u;
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_EINVAL);
    request = test_http_auth_request();
    request.secret_size = 4097u;
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_EPERM);
    check_equal(principal.principal_id[0], '\0');
  }

  it("unwinds remote denial and preserves the HTTP authorization error") {
    static const char denial[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                                 "Connection: close\r\n\r\n";
    flowie_security_auth_request_t request = test_http_auth_request();
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    const flowie_security_auth_provider_t *provider;
    check_equal(flow_tls_test_server_start(&server, (const uint8_t *)denial,
                                          sizeof(denial) - 1u), 0);
    check_equal(test_http_security_configure(server.port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_auth_provider(security);
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_EPERM);
    flow_mtls_test_server_join(&server);
    check_equal(server.status, 0);
    check_equal(principal.principal_id[0], '\0');
  }

  it("unwinds successful ACL decoding without changing the remote policy decision") {
    static const char body[] = "{\"version\":4,\"allowed\":false,\"reason\":\"deny_rule\","
                               "\"policy_version\":42}";
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    flowie_security_request_t request = FLOWIE_SECURITY_REQUEST_INIT;
    flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;
    const flowie_security_authorization_provider_t *provider;
    memcpy(principal.principal_id, "device-a", sizeof("device-a"));
    memcpy(principal.principal_type, "password", sizeof("password"));
    memcpy(principal.domain_id, "booth", sizeof("booth"));
    principal.expires_at = 1900000000u;
    principal.policy_version = 42u;
    request.principal = &principal;
    request.domain_id = "booth";
    request.action = FLOWIE_SECURITY_ACTION_PUBLISH;
    request.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
    request.resource = "booth/devices/device-a/event";
    wire_response = tstr_format("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: {}\r\nConnection: close\r\n\r\n{}", sizeof(body) - 1u, body);
    check_not_null(wire_response);
    check_equal(flow_tls_test_server_start(&server, (const uint8_t *)wire_response,
                                          tstr_len(wire_response)), 0);
    check_equal(test_http_security_configure(server.port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_acl_provider(security);
    check_equal(provider->authorize(provider->ctx, &request, 1u, &decision), SALTS_OK);
    flow_mtls_test_server_join(&server);
    check_equal(server.status, 0);
    check_equal(decision.effect, FLOWIE_SECURITY_DENY);
    check_equal(decision.reason, FLOWIE_SECURITY_REASON_DENY_RULE);
    check_equal(decision.policy_version, 42u);
  }
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
