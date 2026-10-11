#include "flowie_server_http_security_internal.h"

#include "tinytest.h"
#include "cmeta_error.h"
#include <json_parser.h>
#include "tls_test_support.h"
#include <http_server/http.h>
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
  tstr url = tstr_format("https://127.0.0.1:{}/v3/auth", (unsigned int)port);
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

typedef struct test_http_reply {
  const char *body; /* Static test body, borrowed until server stop. */
  unsigned int status;
  int token_received; /* Owner writes; test reads after stop joins the owner. */
} test_http_reply;

static int test_http_reply_handler(void *context, const chttp_server_request_view *request,
                                    chttp_server_response *response) {
  test_http_reply *reply = context;
  const char *token = chttp_server_request_header(request, "Authorization");
  reply->token_received = token && strcmp(token, "Bearer test-only-token") == 0;
  return chttp_server_reply(response, reply->status, "application/json", reply->body,
                            strlen(reply->body));
}

static chttp_server_config test_http_server_config(const cnet_tls_server_config *tls) {
  const chttp_server_config config = {
      .host = "127.0.0.1", .port = 0u, .backlog = 2u,
      .network = {
#if defined(_WIN32)
          .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          .backend = NATIVE_IO_BACKEND_EPOLL,
#else
          .backend = NATIVE_IO_BACKEND_KQUEUE,
#endif
          .connection_capacity = 2u, .command_capacity = 16u,
          .request_capacity = 8u, .completion_batch_capacity = 8u, .event_capacity = 16u,
          .max_send_bytes = 8192u, .receive_buffer_bytes = 4096u,
          .connect_timeout_ms = 1000u, .read_timeout_ms = 1000u, .write_timeout_ms = 1000u,
          .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
          .tls_handshake_timeout_ms = 1000u},
      .route_capacity = 1u, .middleware_capacity = 1u, .max_route_middleware_count = 1u,
      .max_route_param_count = 1u, .max_route_param_bytes = 128u,
      .max_target_bytes = 256u, .max_header_count = 16u, .max_header_bytes = 1024u,
      .max_request_body_bytes = 4096u, .max_response_header_count = 16u,
      .max_response_header_bytes = 1024u, .max_response_body_bytes = 4096u,
      .session_capacity = 1u, .session_entry_capacity = 1u,
      .max_session_key_bytes = 32u, .max_session_value_bytes = 64u,
      .session_idle_timeout_ms = 1000u, .session_cookie_name = "flowie_test",
      .session_cookie_secure = 1, .poll_slice_ms = 2u, .tls = tls};
  return config;
}

spec("Flowie server HTTPS Interceptor") {
  static chttp_server server;
  static flowie_server_http_security_t *security;
  static test_http_reply reply;
  static uint16_t port;
  static char ca_file[1025];
  static char cert_file[1025];
  static char key_file[1025];

  before_each() {
    server = (chttp_server){0};
    security = NULL;
    reply = (test_http_reply){.body = "", .status = 200u};
    port = 0u;
    ca_file[0] = cert_file[0] = key_file[0] = '\0';
    check_equal(test_set_environment("FLOWIE_TEST_NATIVE_HTTP_TOKEN", "test-only-token"), 0);
    check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_equal(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                            key_file, sizeof(key_file)), 0);
    const cnet_tls_server_config tls = {.size = sizeof(tls), .cert_file = cert_file,
        .key_file = key_file, .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
    const chttp_server_config config = test_http_server_config(&tls);
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_route(&server, CHTTP_METHOD_POST, "/v3/auth",
                                   test_http_reply_handler, &reply), SALTS_OK);
  }

  after_each() {
    flowie_server_http_security_destroy(security);
    check_equal(chttp_server_stop(&server, 1000u), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
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
    reply.body = body;
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(test_http_security_configure(port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_auth_provider(security);
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_OK);
    check_equal(chttp_server_stop(&server, 1000u), SALTS_OK);
    check_equal(principal.principal_id, "device-a");
    check_equal(principal.policy_version, 42u);
    check_equal(memcmp(request.secret, "test-only-password", request.secret_size), 0);
    check_true(reply.token_received);
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
    flowie_security_auth_request_t request = test_http_auth_request();
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    const flowie_security_auth_provider_t *provider;
    reply.status = 403u;
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(test_http_security_configure(port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_auth_provider(security);
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_EPERM);
    check_equal(chttp_server_stop(&server, 1000u), SALTS_OK);
    check_true(reply.token_received);
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
    reply.body = body;
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(test_http_security_configure(port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_acl_provider(security);
    check_equal(provider->authorize(provider->ctx, &request, 1u, &decision), SALTS_OK);
    check_equal(chttp_server_stop(&server, 1000u), SALTS_OK);
    check_true(reply.token_received);
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
