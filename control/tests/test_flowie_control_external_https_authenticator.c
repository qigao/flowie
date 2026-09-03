#include "flowie_control_external_https_authenticator_internal.h"

#include "mtls_test_server.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include <json_parser.h>
#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define EXTERNAL_HTTPS_CLIENT_CERT                                                                 \
  "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

typedef struct external_https_secret_fixture_s {
  atomic_int acquire_calls;
  atomic_int release_calls;
  int acquire_status;
  const uint8_t *token;
  size_t token_size;
  uint64_t token_version;
} external_https_secret_fixture_t;

typedef struct external_https_task_s {
  const flowie_control_external_authenticator_t *authenticator;
  flowie_control_external_auth_request_t request;
  flowie_control_external_auth_assertion_t assertion;
  atomic_int done;
  int status;
} external_https_task_t;

typedef struct external_https_network_result_s {
  int status;
  int peer_verified;
  int acquire_calls;
  int release_calls;
  uint8_t request[FLOW_MTLS_TEST_REQUEST_CAPACITY];
  size_t request_size;
  flowie_control_external_auth_assertion_t assertion;
  flowie_control_external_https_authenticator_stats_t stats;
} external_https_network_result_t;

typedef struct external_https_network_options_s {
  const char *response;
  size_t response_size;
  uint32_t response_delay_ms;
  uint32_t timeout_ms;
  uint32_t max_in_flight;
  int configure_ca;
  int configure_client_identity;
  const uint8_t *creation_token;
  size_t creation_token_size;
  const uint8_t *request_token;
  size_t request_token_size;
} external_https_network_options_t;

static const char EXTERNAL_HTTPS_RESPONSE_BODY[] =
    "{\"version\":3,\"authenticated\":true,\"assertion\":{"
    "\"issuer\":\"https://idp.example\",\"domain_id\":\"root-a\","
    "\"subject\":\"tenant-42/device-a\","
    "\"subject_type\":\"device\",\"auth_method\":\"oidc-token\","
    "\"issued_at\":100,\"expires_at\":200,\"revision\":9,\"assurance_level\":2,"
    "\"account_enabled\":true,\"groups\":[\"fleet-a\",\"operators\"]}}";

static int external_https_secret_acquire(void *ctx, const char *reference,
                                         flowie_security_secret_lease_t *lease) {
  static const uint8_t default_token[] = "service-token";
  external_https_secret_fixture_t *fixture = (external_https_secret_fixture_t *)ctx;
  if (!fixture || !reference || strcmp(reference, "env://EXTERNAL_AUTH_TOKEN") != 0 || !lease)
    return SALTS_ENOENT;
  (void)atomic_fetch_add_explicit(&fixture->acquire_calls, 1, memory_order_relaxed);
  if (fixture->acquire_status != SALTS_OK) return fixture->acquire_status;
  lease->bytes = fixture->token ? fixture->token : default_token;
  lease->byte_count = fixture->token ? fixture->token_size : sizeof(default_token) - 1u;
  lease->version = fixture->token ? fixture->token_version : 1u;
  lease->provider_lease = fixture;
  return SALTS_OK;
}

static void external_https_secret_release(void *ctx, flowie_security_secret_lease_t *lease) {
  external_https_secret_fixture_t *fixture = (external_https_secret_fixture_t *)ctx;
  if (fixture)
    (void)atomic_fetch_add_explicit(&fixture->release_calls, 1, memory_order_relaxed);
  if (lease) *lease = (flowie_security_secret_lease_t)FLOWIE_SECURITY_SECRET_LEASE_INIT;
}

static flowie_control_external_auth_request_t external_https_request(void) {
  static const uint8_t secret[] = "signed-token";
  flowie_control_external_auth_request_t request = FLOWIE_CONTROL_EXTERNAL_AUTH_REQUEST_INIT;
  request.domain_id = "root-a";
  request.presented_identity = "external-device";
  request.method = "oidc-token";
  request.secret = secret;
  request.secret_size = sizeof(secret) - 1u;
  request.protocol = "mqtt";
  request.remote_address = "192.0.2.10:1883";
  request.peer_certificate_sha256 = EXTERNAL_HTTPS_CLIENT_CERT;
  return request;
}

static flowie_control_external_https_authenticator_config_t
external_https_config(external_https_secret_fixture_t *fixture) {
  flowie_control_external_https_authenticator_config_t config =
      FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_CONFIG_INIT;
  config.url = "https://idp.example/v1/authenticate";
  config.method = "oidc-token";
  config.service_token_ref = "env://EXTERNAL_AUTH_TOKEN";
  config.key_provider = (flowie_security_key_provider_t){sizeof(flowie_security_key_provider_t),
                                                         fixture, external_https_secret_acquire,
                                                         external_https_secret_release};
  return config;
}

static void external_https_verify_task(void *arg) {
  external_https_task_t *task = (external_https_task_t *)arg;
  task->status =
      task->authenticator->verify(task->authenticator->ctx, &task->request, &task->assertion);
  atomic_store_explicit(&task->done, 1, memory_order_release);
}

static int external_https_http_response(char *buffer, size_t capacity, int status,
                                        const char *reason, const char *content_type,
                                        const char *body) {
  int result;
  size_t body_size = body ? strlen(body) : 0u;
  if (!buffer || capacity == 0u || !reason) return -1;
  result = snprintf(buffer, capacity,
                    "HTTP/1.1 %d %s\r\n%s%s%sContent-Length: %zu\r\n"
                    "Connection: close\r\n\r\n%s",
                    status, reason, content_type ? "Content-Type: " : "",
                    content_type ? content_type : "", content_type ? "\r\n" : "", body_size,
                    body ? body : "");
  return result > 0 && (size_t)result < capacity ? result : -1;
}

static external_https_network_options_t external_https_network_options(const char *response,
                                                                       size_t response_size) {
  static const uint8_t token[] = "service-token";
  external_https_network_options_t options;
  memset(&options, 0, sizeof(options));
  options.response = response;
  options.response_size = response_size;
  options.timeout_ms = 3000u;
  options.max_in_flight = 64u;
  options.configure_ca = 1;
  options.configure_client_identity = 1;
  options.creation_token = token;
  options.creation_token_size = sizeof(token) - 1u;
  options.request_token = token;
  options.request_token_size = sizeof(token) - 1u;
  return options;
}

static int external_https_run_mtls(const external_https_network_options_t *options,
                                   external_https_network_result_t *result) {
  char cert_file[512] = {0};
  char key_file[512] = {0};
  char url[160];
  flow_mtls_test_server_t server;
  external_https_secret_fixture_t fixture;
  flowie_control_external_https_authenticator_config_t config = external_https_config(&fixture);
  flowie_control_external_https_authenticator_t *authenticator = NULL;
  external_https_task_t task;
  salts_thread_t worker = {0};
  int worker_started = 0;
  int rc = SALTS_EIO;

  if (!options || (!options->response && options->response_size != 0u) ||
      options->timeout_ms == 0u || options->max_in_flight == 0u || !options->creation_token ||
      options->creation_token_size == 0u || !options->request_token ||
      options->request_token_size == 0u || !result)
    return SALTS_EINVAL;
  memset(result, 0, sizeof(*result));
  memset(&fixture, 0, sizeof(fixture));
  atomic_init(&fixture.acquire_calls, 0);
  atomic_init(&fixture.release_calls, 0);
  fixture.token = options->creation_token;
  fixture.token_size = options->creation_token_size;
  fixture.token_version = 1u;
  result->status = SALTS_EIO;
  result->assertion =
      (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  result->stats = (flowie_control_external_https_authenticator_stats_t)
      FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_STATS_INIT;
  memset(&server, 0, sizeof(server));
  memset(&task, 0, sizeof(task));
  atomic_init(&task.done, 0);
  task.status = SALTS_EBUSY;
  if (tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)) != 0)
    goto done;
  if (flow_mtls_test_server_start_delayed(&server, (const uint8_t *)options->response,
                                          options->response_size, options->response_delay_ms) != 0)
    goto done;
  if (snprintf(url, sizeof(url), "https://localhost:%u/v1/authenticate", server.port) <= 0)
    goto done;
  config.url = url;
  config.timeout_ms = options->timeout_ms;
  config.max_in_flight = options->max_in_flight;
  config.tls.ca_file = options->configure_ca ? cert_file : NULL;
  config.tls.client_cert_file = options->configure_client_identity ? cert_file : NULL;
  config.tls.client_key_file = options->configure_client_identity ? key_file : NULL;
  rc = flowie_control_external_https_authenticator_create(&config, &authenticator);
  if (rc != SALTS_OK) goto done;
  fixture.token = options->request_token;
  fixture.token_size = options->request_token_size;
  fixture.token_version = 3u;
  task.authenticator = flowie_control_external_https_authenticator_interface(authenticator);
  task.request = external_https_request();
  task.assertion =
      (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  rc = salts_thread_create(&worker, external_https_verify_task, &task);
  if (rc != SALTS_OK) goto done;
  worker_started = 1;
  rc = salts_thread_join(&worker);
  worker_started = 0;
  if (rc != SALTS_OK) goto done;
  result->status = task.status;
  result->assertion = task.assertion;
  rc = SALTS_OK;

done:
  if (worker_started) (void)salts_thread_join(&worker);
  if (authenticator &&
      flowie_control_external_https_authenticator_get_stats(authenticator, &result->stats) !=
          SALTS_OK &&
      rc == SALTS_OK)
    rc = SALTS_EIO;
  flowie_control_external_https_authenticator_destroy(authenticator);
  flow_mtls_test_server_join(&server);
  result->peer_verified = server.peer_verified;
  result->request_size = server.request_size;
  if (server.request_size != 0u) memcpy(result->request, server.request, server.request_size + 1u);
  result->acquire_calls = atomic_load_explicit(&fixture.acquire_calls, memory_order_relaxed);
  result->release_calls = atomic_load_explicit(&fixture.release_calls, memory_order_relaxed);
  tls_test_remove_file(key_file);
  tls_test_remove_file(cert_file);
  return rc;
}

spec("Flowie control external HTTPS authenticator") {
  it("strictly decodes one typed assertion and explicit denial") {
    flowie_control_external_auth_assertion_t assertion =
        FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
    static const char denied[] = "{\"version\":3,\"authenticated\":false}";

    check_equal(flowie_control_external_https_decode_response(
                    EXTERNAL_HTTPS_RESPONSE_BODY, sizeof(EXTERNAL_HTTPS_RESPONSE_BODY) - 1u,
                    "oidc-token", &assertion),
                SALTS_OK);
    check_equal(assertion.issuer, "https://idp.example");
    check_equal(assertion.domain_id, "root-a");
    check_equal(assertion.subject, "tenant-42/device-a");
    check_equal(assertion.subject_type, "device");
    check_equal(assertion.auth_method, "oidc-token");
    check_equal(assertion.issued_at, 100u);
    check_equal(assertion.expires_at, 200u);
    check_equal(assertion.revision, 9u);
    check_equal(assertion.assurance_level, FLOWIE_CONTROL_EXTERNAL_ASSURANCE_MULTI_FACTOR);
    check_true(assertion.account_enabled);
    check_equal(assertion.external_group_count, 2u);
    check_equal(assertion.external_groups[0], "fleet-a");
    check_equal(assertion.external_groups[1], "operators");

    check_equal(flowie_control_external_https_decode_response(denied, sizeof(denied) - 1u,
                                                              "oidc-token", &assertion),
                SALTS_EPERM);
    check_equal(assertion.subject, "");
  }

  it("rejects unknown fields, duplicate groups, wrong methods, and noncanonical numbers") {
    flowie_control_external_auth_assertion_t assertion =
        FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
    static const char unknown[] = "{\"version\":3,\"authenticated\":false,\"reason\":\"hidden\"}";
    static const char duplicate_field[] =
        "{\"version\":3,\"authenticated\":false,\"authenticated\":false}";
    static const char duplicate_group[] =
        "{\"version\":3,\"authenticated\":true,\"assertion\":{"
        "\"issuer\":\"idp\",\"domain_id\":\"root-a\",\"subject\":\"device-a\","
        "\"subject_type\":\"device\","
        "\"auth_method\":\"oidc-token\",\"issued_at\":100,\"expires_at\":200,"
        "\"revision\":9,\"assurance_level\":2,\"account_enabled\":true,"
        "\"groups\":[\"fleet-a\",\"fleet-a\"]}}";
    static const char leading_zero[] = "{\"version\":01,\"authenticated\":false}";
    static const char overflow[] = "{\"version\":18446744073709551616,\"authenticated\":false}";

    check_equal(flowie_control_external_https_decode_response(unknown, sizeof(unknown) - 1u,
                                                              "oidc-token", &assertion),
                SALTS_EPROTO);
    check_equal(flowie_control_external_https_decode_response(
                    duplicate_field, sizeof(duplicate_field) - 1u, "oidc-token", &assertion),
                SALTS_EPROTO);
    check_equal(flowie_control_external_https_decode_response(
                    duplicate_group, sizeof(duplicate_group) - 1u, "oidc-token", &assertion),
                SALTS_EPROTO);
    check_equal(flowie_control_external_https_decode_response(
                    EXTERNAL_HTTPS_RESPONSE_BODY, sizeof(EXTERNAL_HTTPS_RESPONSE_BODY) - 1u,
                    "password", &assertion),
                SALTS_EPROTO);
    check_equal(flowie_control_external_https_decode_response(
                    leading_zero, sizeof(leading_zero) - 1u, "oidc-token", &assertion),
                SALTS_EPROTO);
    check_equal(flowie_control_external_https_decode_response(overflow, sizeof(overflow) - 1u,
                                                              "oidc-token", &assertion),
                SALTS_EPROTO);
  }

  it("encodes only the bounded versioned request fields") {
    flowie_control_external_auth_request_t request = external_https_request();
    json_value_t *document = NULL;
    char *body = NULL;
    size_t body_size = 0u;

    check_equal(flowie_control_external_https_encode_request(&request, &body, &body_size),
                SALTS_OK);
    check_not_null(body);
    document = json_parse(body, body_size);
    check_not_null(document);
    check_not_null(document);
    check_equal(json_object_size(document), 8u);
    check_within(json_number(json_object_get(document, "version")), 3.0, 0.001);
    check_equal(json_string(json_object_get(document, "domain")), "root-a");
    check_equal(json_string(json_object_get(document, "identity")), "external-device");
    check_equal(json_string(json_object_get(document, "secret_base64")),
                "c2lnbmVkLXRva2Vu");
    check_equal(json_string(json_object_get(document, "protocol")), "mqtt");
    check_equal(json_string(json_object_get(document, "remote_address")),
                "192.0.2.10:1883");
    check_equal(json_string(json_object_get(document, "peer_certificate_sha256")),
                EXTERNAL_HTTPS_CLIENT_CERT);

    json_free(document);
    json_serialize_free(body);
    document = NULL;
    body = NULL;
    body_size = 0u;
    request.domain_id = "";
    check_equal(flowie_control_external_https_encode_request(&request, &body, &body_size),
                SALTS_OK);
    document = json_parse(body, body_size);
    check_not_null(document);
    check_equal(json_string(json_object_get(document, "domain")), "");
    json_free(document);
    json_serialize_free(body);
  }

  it("rejects a non-canonical MQTT client certificate fingerprint before HTTPS") {
    flowie_control_external_auth_request_t request = external_https_request();
    char *body = NULL;
    size_t body_size = 0u;

    request.peer_certificate_sha256 =
        "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    check_equal(flowie_control_external_https_encode_request(&request, &body, &body_size),
                SALTS_EINVAL);
    check_null(body);
    check_equal(body_size, 0u);
  }

  it("fails fast on insecure URLs, incomplete TLS identity, and invalid requests") {
    external_https_secret_fixture_t fixture = {0};
    external_https_secret_fixture_t invalid_tls_fixture = {0};
    flowie_control_external_https_authenticator_config_t config = external_https_config(&fixture);
    flowie_control_external_https_authenticator_t *authenticator = NULL;
    const flowie_control_external_authenticator_t *interface;
    flowie_control_external_auth_request_t request = external_https_request();
    flowie_control_external_auth_assertion_t assertion =
        FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
    flowie_control_external_https_authenticator_stats_t stats =
        FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_STATS_INIT;

    atomic_init(&fixture.acquire_calls, 0);
    atomic_init(&fixture.release_calls, 0);
    atomic_init(&invalid_tls_fixture.acquire_calls, 0);
    atomic_init(&invalid_tls_fixture.release_calls, 0);

    config.url = "http://idp.example/v1/authenticate";
    check_equal(flowie_control_external_https_authenticator_create(&config, &authenticator),
                SALTS_EINVAL);
    check_null(authenticator);
    config = external_https_config(&invalid_tls_fixture);
    config.tls.client_cert_file = "missing-client-cert.pem";
    config.tls.client_key_file = "missing-client-key.pem";
    config.tls.client_key_password_ref = "env://EXTERNAL_AUTH_TOKEN";
    check_equal(flowie_control_external_https_authenticator_create(&config, &authenticator),
                SALTS_EIO);
    check_null(authenticator);
    check_equal(atomic_load_explicit(&invalid_tls_fixture.acquire_calls, memory_order_relaxed), 2);
    check_equal(atomic_load_explicit(&invalid_tls_fixture.release_calls, memory_order_relaxed), 2);
    config = external_https_config(&fixture);
    config.tls.client_cert_file = "client.pem";
    check_equal(flowie_control_external_https_authenticator_create(&config, &authenticator),
                SALTS_EINVAL);
    check_null(authenticator);
    config = external_https_config(&fixture);
    check_equal(flowie_control_external_https_authenticator_create(&config, &authenticator),
                SALTS_OK);
    check_not_null(authenticator);
    interface = flowie_control_external_https_authenticator_interface(authenticator);
    check_not_null(interface);
    check_equal(flowie_control_external_authenticator_validate(interface), SALTS_OK);
    request.protocol = NULL;
    check_equal(interface->verify(interface->ctx, &request, &assertion), SALTS_EINVAL);
    check_equal(atomic_load_explicit(&fixture.acquire_calls, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&fixture.release_calls, memory_order_relaxed), 1);
    check_equal(flowie_control_external_https_authenticator_get_stats(authenticator, &stats),
                SALTS_OK);
    check_equal(stats.started_requests, 0u);
    stats.size = 0u;
    check_equal(flowie_control_external_https_authenticator_get_stats(authenticator, &stats),
                SALTS_EINVAL);

    flowie_control_external_https_authenticator_destroy(authenticator);
  }

  it("performs one CHTTP mTLS request and returns the remote assertion") {
    char response[4096];
    external_https_network_result_t result;
    external_https_network_options_t options;
    int response_size;

    response_size = external_https_http_response(response, sizeof(response), 200, "OK",
                                                 "application/json", EXTERNAL_HTTPS_RESPONSE_BODY);
    check_greater(response_size, 0);
    options = external_https_network_options(response, (size_t)response_size);
    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_OK);
    check_true(result.peer_verified);
    check_equal(result.assertion.subject, "tenant-42/device-a");
    check_equal(result.assertion.external_group_count, 2u);
    check_equal(result.acquire_calls, 2u);
    check_equal(result.release_calls, 2u);
    check_equal(result.stats.started_requests, 1u);
    check_equal(result.stats.in_flight, 0u);
    check_equal(result.stats.succeeded, 1u);
  }

  it("maps remote authentication rejection to permission denied") {
    static const char response[] =
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    external_https_network_result_t result;
    external_https_network_options_t options =
        external_https_network_options(response, sizeof(response) - 1u);

    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_EPERM);
    check_true(result.peer_verified);
    check_equal(result.stats.started_requests, 1u);
    check_equal(result.stats.denied, 1u);
  }

  it("maps remote overload to busy without retrying") {
    static const char response[] =
        "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    external_https_network_result_t result;
    external_https_network_options_t options =
        external_https_network_options(response, sizeof(response) - 1u);

    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_EBUSY);
    check_true(result.peer_verified);
    check_equal(result.acquire_calls, 2u);
    check_equal(result.stats.remote_overload, 1u);
  }

  it("distinguishes a third-party server failure from a protocol failure") {
    static const char response[] =
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    external_https_network_result_t result;
    external_https_network_options_t options =
        external_https_network_options(response, sizeof(response) - 1u);

    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_EIO);
    check_true(result.peer_verified);
    check_equal(result.stats.remote_server_failures, 1u);
    check_equal(result.stats.protocol_failures, 0u);
  }

  it("fails closed on a malformed JSON assertion") {
    static const char invalid_body[] = "{\"version\":3,\"authenticated\":";
    char response[512];
    external_https_network_result_t result;
    external_https_network_options_t options;
    int response_size = external_https_http_response(response, sizeof(response), 200, "OK",
                                                     "application/json", invalid_body);

    check_greater(response_size, 0);
    options = external_https_network_options(response, (size_t)response_size);
    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_EPROTO);
    check_true(result.peer_verified);
    check_equal(result.stats.protocol_failures, 1u);
  }

  it("fails closed when the third-party response exceeds its timeout") {
    char response[4096];
    external_https_network_result_t result;
    external_https_network_options_t options;
    int response_size = external_https_http_response(
        response, sizeof(response), 200, "OK", "application/json", EXTERNAL_HTTPS_RESPONSE_BODY);

    check_greater(response_size, 0);
    options = external_https_network_options(response, (size_t)response_size);
    options.response_delay_ms = 250u;
    options.timeout_ms = 50u;
    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_EIO);
    check_true(result.peer_verified);
    check_equal(result.stats.transport_failures, 1u);
  }

  it("fails closed when the TLS peer closes before an HTTP response") {
    external_https_network_result_t result;
    external_https_network_options_t options = external_https_network_options(NULL, 0u);

    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_EIO);
    check_true(result.peer_verified);
    check_greater(result.request_size, 0u);
    check_equal(result.stats.transport_failures, 1u);
  }

  it("fails the handshake when the third-party service requires an absent client certificate") {
    char response[4096];
    external_https_network_result_t result;
    external_https_network_options_t options;
    int response_size = external_https_http_response(
        response, sizeof(response), 200, "OK", "application/json", EXTERNAL_HTTPS_RESPONSE_BODY);

    check_greater(response_size, 0);
    options = external_https_network_options(response, (size_t)response_size);
    options.configure_client_identity = 0;
    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_EIO);
    check_false(result.peer_verified);
    check_equal(result.request_size, 0u);
    check_equal(result.stats.transport_failures, 1u);
  }

  it("fails the handshake when the third-party server certificate is untrusted") {
    char response[4096];
    external_https_network_result_t result;
    external_https_network_options_t options;
    int response_size = external_https_http_response(
        response, sizeof(response), 200, "OK", "application/json", EXTERNAL_HTTPS_RESPONSE_BODY);

    check_greater(response_size, 0);
    options = external_https_network_options(response, (size_t)response_size);
    options.configure_ca = 0;
    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_EIO);
    check_equal(result.request_size, 0u);
    check_equal(result.stats.transport_failures, 1u);
  }

  it("loads the service token again for each outgoing request") {
    static const uint8_t creation_token[] = "initial-token";
    static const uint8_t request_token[] = "rotated-token";
    char response[4096];
    external_https_network_result_t result;
    external_https_network_options_t options;
    int response_size = external_https_http_response(
        response, sizeof(response), 200, "OK", "application/json", EXTERNAL_HTTPS_RESPONSE_BODY);

    check_greater(response_size, 0);
    options = external_https_network_options(response, (size_t)response_size);
    options.creation_token = creation_token;
    options.creation_token_size = sizeof(creation_token) - 1u;
    options.request_token = request_token;
    options.request_token_size = sizeof(request_token) - 1u;
    check_equal(external_https_run_mtls(&options, &result), SALTS_OK);
    check_equal(result.status, SALTS_OK);
    check_equal(result.acquire_calls, 2u);
    check_contains((const char *)result.request, "Authorization: Bearer rotated-token\r\n");
    check_null(strstr((const char *)result.request, "Authorization: Bearer initial-token"));
  }

  it("reports a runtime service-token provider failure without attempting network access") {
    external_https_secret_fixture_t fixture = {0};
    flowie_control_external_https_authenticator_config_t config = external_https_config(&fixture);
    flowie_control_external_https_authenticator_t *authenticator = NULL;
    external_https_task_t task;
    salts_thread_t worker = {0};
    flowie_control_external_https_authenticator_stats_t stats =
        FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_STATS_INIT;

    memset(&task, 0, sizeof(task));
    atomic_init(&fixture.acquire_calls, 0);
    atomic_init(&fixture.release_calls, 0);
    atomic_init(&task.done, 0);
    check_equal(flowie_control_external_https_authenticator_create(&config, &authenticator),
                SALTS_OK);
    check_not_null(authenticator);
    fixture.acquire_status = SALTS_EIO;
    task.authenticator = flowie_control_external_https_authenticator_interface(authenticator);
    task.request = external_https_request();
    task.assertion =
        (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
    check_equal(salts_thread_create(&worker, external_https_verify_task, &task), SALTS_OK);
    check_equal(salts_thread_join(&worker), SALTS_OK);

    check_equal(task.status, SALTS_EIO);
    check_equal(flowie_control_external_https_authenticator_get_stats(authenticator, &stats),
                SALTS_OK);
    check_equal(stats.started_requests, 1u);
    check_equal(stats.in_flight, 0u);
    check_equal(stats.local_failures, 1u);
    check_equal(stats.transport_failures, 0u);
    check_equal(atomic_load_explicit(&fixture.acquire_calls, memory_order_relaxed), 2);

    flowie_control_external_https_authenticator_destroy(authenticator);
  }

  it("rejects excess concurrent authentication before secret or network access") {
    char cert_file[512] = {0};
    char key_file[512] = {0};
    char response[4096];
    char url[160];
    flow_mtls_test_server_t server;
    external_https_secret_fixture_t fixture = {0};
    flowie_control_external_https_authenticator_config_t config = external_https_config(&fixture);
    flowie_control_external_https_authenticator_t *authenticator = NULL;
    external_https_task_t first;
    external_https_task_t second;
    salts_thread_t first_worker = {0};
    salts_thread_t second_worker = {0};
    flowie_control_external_https_authenticator_stats_t stats =
        FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_STATS_INIT;
    int response_size;

    memset(&server, 0, sizeof(server));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    atomic_init(&fixture.acquire_calls, 0);
    atomic_init(&fixture.release_calls, 0);
    atomic_init(&first.done, 0);
    atomic_init(&second.done, 0);
    first.status = SALTS_EIO;
    second.status = SALTS_EIO;
    response_size = external_https_http_response(response, sizeof(response), 200, "OK",
                                                 "application/json", EXTERNAL_HTTPS_RESPONSE_BODY);
    check_greater(response_size, 0);
    check_equal(
        tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)), 0);
    check_equal(flow_mtls_test_server_start_delayed(&server, (const uint8_t *)response,
                                                    (size_t)response_size, 1000u),
                0);
    check_greater(snprintf(url, sizeof(url), "https://localhost:%u/v1/authenticate", server.port),
                  0);
    config.url = url;
    config.max_in_flight = 1u;
    config.tls.ca_file = cert_file;
    config.tls.client_cert_file = cert_file;
    config.tls.client_key_file = key_file;
    check_equal(flowie_control_external_https_authenticator_create(&config, &authenticator),
                SALTS_OK);
    check_not_null(authenticator);
    first.authenticator = flowie_control_external_https_authenticator_interface(authenticator);
    first.request = external_https_request();
    first.assertion =
        (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
    second.authenticator = first.authenticator;
    second.request = external_https_request();
    second.assertion =
        (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
    check_equal(salts_thread_create(&first_worker, external_https_verify_task, &first), SALTS_OK);
    while (atomic_load_explicit(&fixture.acquire_calls, memory_order_relaxed) < 2 &&
           !atomic_load_explicit(&first.done, memory_order_acquire))
      salts_thread_yield();
    check_false(atomic_load_explicit(&first.done, memory_order_acquire));
    check_equal(atomic_load_explicit(&fixture.acquire_calls, memory_order_relaxed), 2);
    check_equal(salts_thread_create(&second_worker, external_https_verify_task, &second), SALTS_OK);
    check_equal(salts_thread_join(&second_worker), SALTS_OK);
    check_equal(second.status, SALTS_EBUSY);
    check_equal(atomic_load_explicit(&fixture.acquire_calls, memory_order_relaxed), 2);
    check_equal(salts_thread_join(&first_worker), SALTS_OK);
    check_equal(first.status, SALTS_OK);
    check_true(server.peer_verified);
    check_equal(atomic_load_explicit(&fixture.acquire_calls, memory_order_relaxed), 2);
    check_equal(atomic_load_explicit(&fixture.release_calls, memory_order_relaxed), 2);
    check_equal(flowie_control_external_https_authenticator_get_stats(authenticator, &stats),
                SALTS_OK);
    check_equal(stats.started_requests, 2u);
    check_equal(stats.in_flight, 0u);
    check_equal(stats.succeeded, 1u);
    check_equal(stats.local_overload, 1u);

    flowie_control_external_https_authenticator_destroy(authenticator);
    flow_mtls_test_server_join(&server);
    tls_test_remove_file(key_file);
    tls_test_remove_file(cert_file);
  }
}
