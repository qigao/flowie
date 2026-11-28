#include "flowie_server_http_security_internal.h"
#include "flowie_workflow_https_sink_internal.h"

#include "tinytest.h"
#include <core/error.h>
#include <json_parser.h>
#include "tls_test_support.h"
#include <http_server/http.h>
#include <chttp_app/application.h>
#include <cmeta/data_reflect.h>
#include <core/fmt.h>

#include <string.h>
#include <stdio.h>

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
  int token_received; /* Application worker writes; test reads after close joins it. */
  uint16_t workflow_ports[2];
  size_t workflow_posts;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  chttp_controller controller;
  chttp_action action;
} test_http_reply;
cmeta_reflect_data(test_http_reply, "flowie.test.HttpReply", cmeta_field(int, token_received));
cmeta_component_empty(FlowieTestHttpReply);
CHTTP_ACTION(test_http_reply_handler);

typedef struct test_workflow_outcomes {
  chttp_client *client;
  chttp_client_config client_config;
  size_t count;
  int statuses[2];
} test_workflow_outcomes;

static int test_workflow_start(void *context) {
  test_workflow_outcomes *fixture = context;
  return chttp_client_init(fixture->client, &fixture->client_config);
}

static int test_workflow_stop(void *context) {
  test_workflow_outcomes *fixture = context;
  return chttp_client_destroy(fixture->client, 1000u);
}

static int test_workflow_decide(void *context, const flowie_workflow_event_t *event,
                                flowie_workflow_decision_t *decision) {
  (void)context;
  *decision = (flowie_workflow_decision_t){.generation = event->generation, .event_id = event->id,
      .action = FLOWIE_WORKFLOW_FORWARD, .sink_size = sizeof("loopback") - 1u, .sink = "loopback"};
  return SALTS_OK;
}

static void test_workflow_complete(void *context, const flowie_workflow_event_t *event,
                                   flowie_workflow_phase_t phase, int status) {
  test_workflow_outcomes *outcomes = context;
  (void)event;
  if (outcomes->count < 2u) {
    outcomes->statuses[outcomes->count] = phase == FLOWIE_WORKFLOW_DELIVERY ? status : SALTS_EINVAL;
  }
  ++outcomes->count;
}

static cmeta_status test_http_component_create(void *user, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *dependencies, size_t count,
    cmeta_object_ref *out) {
  (void)data; (void)config; (void)dependencies;
  if (count != 0u) return CMETA_INVALID_ARGUMENT;
  return cmeta_object_borrow(out, user, cmeta_reflected_data(test_http_reply), NULL);
}


int test_http_reply_handler(void *context, const chttp_invocation *invocation) {
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  test_http_reply *reply = context;
  const char *token = chttp_server_request_header(request, "Authorization");
  if (request->body_size == 2u && request->peer && reply->workflow_posts < 2u) {
    const unsigned char *body = request->body;
    if (body[0] == 0u && body[1] == 0xa5u)
      reply->workflow_ports[reply->workflow_posts++] = request->peer->port;
  }
  reply->token_received = token && strcmp(token, "Bearer test-only-token") == 0;
  return chttp_invocation_respond(invocation, reply->status, "application/json", reply->body,
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
  static chttp_application server;
  static flowie_server_http_security_t *security;
  static test_http_reply reply;
  static chttp_client workflow_client;
  static chttp_tls_profile workflow_tls;
  static flowie_workflow_host_t *workflow_host;
  static flowie_workflow_https_sink_t workflow_sink;
  static test_workflow_outcomes workflow_outcomes;
  static char workflow_uri[64];
  static char workflow_authority[32];
  static uint16_t port;
  static char ca_file[1025];
  static char cert_file[1025];
  static char key_file[1025];

  before_each() {
    server = (chttp_application){0};
    security = NULL;
    reply = (test_http_reply){.body = "", .status = 200u};
    workflow_client = (chttp_client){0};
    workflow_tls = (chttp_tls_profile){0};
    workflow_host = NULL;
    workflow_outcomes = (test_workflow_outcomes){0};
    port = 0u;
    ca_file[0] = cert_file[0] = key_file[0] = '\0';
    check_equal(test_set_environment("FLOWIE_TEST_NATIVE_HTTP_TOKEN", "test-only-token"), 0);
    check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_equal(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                            key_file, sizeof(key_file)), 0);
    const cnet_tls_server_config tls = {.size = sizeof(tls), .cert_file = cert_file,
        .key_file = key_file, .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
    chttp_application_config config = chttp_application_config_default();
    config.http = test_http_server_config(&tls);
    config.workers = (chttp_worker_config){1u, 2u, 64u * 1024u, 8192u};
    reply.provider = (salts_configurator_provider_binding){
        sizeof(reply.provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(FlowieTestHttpReply), &reply, NULL, test_http_component_create, NULL, NULL};
    reply.deployment = (salts_configurator_deployment){&reply.provider, NULL, NULL};
    reply.action = (chttp_action)CHTTP_POST("/v3/auth", test_http_reply_handler);
    reply.controller = (chttp_controller){
        .component_id = "FlowieTestHttpReply", .receiver = cmeta_reflected_data(test_http_reply),
        .actions = &reply.action, .action_count = 1u};
    config.components = &reply.deployment;
    config.component_count = 1u;
    config.dependency_capacity = 1u;
    config.controllers = &reply.controller;
    config.controller_count = 1u;
    check_equal(chttp_application_init(&server, &config), SALTS_OK);
  }

  after_each() {
    check_equal(flowie_workflow_host_destroy(&workflow_host), SALTS_OK);
    check_null(workflow_client.impl);
    if (workflow_tls.impl) check_equal(chttp_tls_profile_destroy(&workflow_tls), SALTS_OK);
    flowie_server_http_security_destroy(security);
    check_equal(chttp_application_close(&server, 1000u), SALTS_OK);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
    if (ca_file[0]) tls_test_remove_file(ca_file);
    check_equal(test_set_environment("FLOWIE_TEST_NATIVE_HTTP_TOKEN", NULL), 0);
  }

  it("forwards two workflow events over one verified HTTPS connection") {
    check_equal(chttp_application_start(&server), SALTS_OK);
    check_equal(chttp_application_port(&server, &port), SALTS_OK);
    check_true(snprintf(workflow_authority, sizeof(workflow_authority), "127.0.0.1:%u",
                        (unsigned int)port) > 0);
    check_true(snprintf(workflow_uri, sizeof(workflow_uri), "tls://%s", workflow_authority) > 0);
    const cnet_tls_client_config tls = {.size = sizeof(tls), .ca_file = ca_file,
                                       .server_name = "127.0.0.1"};
    check_equal(chttp_tls_profile_init(&workflow_tls, &tls), SALTS_OK);
    workflow_outcomes.client = &workflow_client;
    workflow_outcomes.client_config = (chttp_client_config){
        .network = {
            .backend = test_http_server_config(NULL).network.backend,
            .connection_capacity = 1u, .command_capacity = 8u, .request_capacity = 4u,
            .completion_batch_capacity = 4u, .event_capacity = 8u, .max_send_bytes = 8192u,
            .receive_buffer_bytes = 4096u, .connect_timeout_ms = 1000u, .read_timeout_ms = 1000u,
            .write_timeout_ms = 1000u, .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
            .tls_handshake_timeout_ms = 1000u},
        .request_capacity = 1u, .max_start_line_bytes = 2048u, .max_header_count = 16u,
        .max_header_bytes = 1024u, .max_request_body_bytes = 4096u, .max_response_body_bytes = 4096u,
        .max_informational_responses = 4u};
    workflow_sink = (flowie_workflow_https_sink_t){.client = &workflow_client,
        .request = {.connection_uri = workflow_uri, .authority = workflow_authority,
            .target = "/v3/auth", .timeout_ms = 1000u, .tls = &workflow_tls,
            .protocol = CHTTP_HTTP_1_1}};
    const flowie_workflow_sink_t binding = {"loopback", flowie_workflow_https_deliver, &workflow_sink};
    const flowie_workflow_host_config_t config = {.generation = 1u, .event_capacity = 2u,
        .max_event_bytes = 16u, .max_pending_bytes = 32u, .sinks = &binding,
        .sink_count = 1u, .start = test_workflow_start, .stop = test_workflow_stop,
        .decide = test_workflow_decide, .complete = test_workflow_complete,
        .context = &workflow_outcomes};
    check_equal(flowie_workflow_host_create(&config, &workflow_host), SALTS_OK);
    flowie_workflow_event_t event = {.generation = 1u, .id = 1u, .topic = "input", .topic_size = 5u,
                                    .payload = "\0\xA5", .payload_size = 2u};
    check_equal(flowie_workflow_host_submit(workflow_host, &event), SALTS_OK);
    event.id = 2u;
    check_equal(flowie_workflow_host_submit(workflow_host, &event), SALTS_OK);
    check_equal(flowie_workflow_host_destroy(&workflow_host), SALTS_OK);
    check_equal(chttp_application_close(&server, 1000u), SALTS_OK);
    check_equal(workflow_outcomes.count, 2u);
    check_equal(workflow_outcomes.statuses[0], SALTS_OK);
    check_equal(workflow_outcomes.statuses[1], SALTS_OK);
    check_equal(reply.workflow_posts, 2u);
    check_true(reply.workflow_ports[0] != 0u);
    check_equal(reply.workflow_ports[0], reply.workflow_ports[1]);
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
    check_equal(chttp_application_start(&server), SALTS_OK);
    check_equal(chttp_application_port(&server, &port), SALTS_OK);
    check_equal(test_http_security_configure(port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_auth_provider(security);
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_OK);
    check_equal(chttp_application_close(&server, 1000u), SALTS_OK);
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
    check_equal(chttp_application_start(&server), SALTS_OK);
    check_equal(chttp_application_port(&server, &port), SALTS_OK);
    check_equal(test_http_security_configure(port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_auth_provider(security);
    check_equal(provider->authenticate(provider->ctx, &request, &principal), SALTS_EPERM);
    check_equal(chttp_application_close(&server, 1000u), SALTS_OK);
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
    check_equal(chttp_application_start(&server), SALTS_OK);
    check_equal(chttp_application_port(&server, &port), SALTS_OK);
    check_equal(test_http_security_configure(port, ca_file, &security), SALTS_OK);
    provider = flowie_server_http_security_acl_provider(security);
    check_equal(provider->authorize(provider->ctx, &request, 1u, &decision), SALTS_OK);
    check_equal(chttp_application_close(&server, 1000u), SALTS_OK);
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
