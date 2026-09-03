#include "flowie_control_config_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <stdio.h>
#include <string.h>

#define ADMIN_FINGERPRINT "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static const char valid_config[] = "version: 1\n"
                                   "listener:\n"
                                   "  tls:\n"
                                   "    cert_file: certs/control.crt\n"
                                   "    key_file: certs/control.key\n"
                                   "    key_password_ref: env://FLOWIE_CONTROL_KEY_PASSWORD\n"
                                   "storage:\n"
                                   "  turbodb:\n"
                                   "    driver: sqlite\n"
                                   "    options:\n"
                                   "      filename: data/flowie-control.db\n"
                                   "management:\n"
                                   "  session:\n"
                                   "    capacity: 512\n"
                                   "    max_sessions_per_principal: 7\n"
                                   "    ttl_seconds: 1800\n"
                                   "  login_executor:\n"
                                   "    workers: 3\n"
                                   "    queue_capacity: 64\n"
                                   "    deadline_ms: 8000\n"
                                   "dashboard:\n"
                                   "  enabled: true\n"
                                   "auth:\n"
                                   "  enabled: false\n";

static const char valid_external_https_config[] =
    "version: 1\n"
    "listener:\n"
    "  tls:\n"
    "    cert_file: cert.pem\n"
    "    key_file: key.pem\n"
    "storage:\n"
    "  turbodb:\n"
    "    driver: sqlite\n"
    "    options:\n"
    "      filename: control.db\n"
    "management:\n"
    "  session:\n"
    "    capacity: 1024\n"
    "auth:\n"
    "  enabled: true\n"
    "  listener_id: flowie-control-auth\n"
    "  method: bearer\n"
    "  external_https:\n"
    "    url: https://auth.example/v1/assert\n"
    "    service_token_ref: env://FLOWIE_THIRD_PARTY_AUTH_TOKEN\n"
    "    trusted_issuer: https://identity.example\n"
    "    subject_type: device\n"
    "    timeout_ms: 2500\n"
    "    max_response_size: 8192\n"
    "    max_in_flight: 32\n"
    "    tls:\n"
    "      ca_file: third-party-ca.pem\n"
    "      client_cert_file: flowie-client.pem\n"
    "      client_key_file: flowie-client-key.pem\n"
    "      client_key_password_ref: env://FLOWIE_THIRD_PARTY_KEY_PASSWORD\n";

static const char valid_jwt_jwks_config[] =
    "version: 1\n"
    "listener:\n"
    "  tls:\n"
    "    cert_file: cert.pem\n"
    "    key_file: key.pem\n"
    "storage:\n"
    "  turbodb:\n"
    "    driver: sqlite\n"
    "    options:\n"
    "      filename: control.db\n"
    "management:\n"
    "  session:\n"
    "    capacity: 1024\n"
    "auth:\n"
    "  enabled: true\n"
    "  listener_id: flowie-control-auth\n"
    "  method: bearer\n"
    "  jwt_jwks:\n"
    "    url: https://identity.example/.well-known/jwks.json\n"
    "    trusted_issuer: https://identity.example\n"
    "    audience: flowie\n"
    "    subject_type: device\n"
    "    algorithm: EdDSA\n"
    "    timeout_ms: 2400\n"
    "    max_response_size: 32768\n"
    "    max_keys: 8\n"
    "    max_token_size: 3072\n"
    "    refresh_interval_seconds: 120\n"
    "    clock_skew_seconds: 15\n"
    "    executor:\n"
    "      workers: 3\n"
    "      queue_capacity: 48\n"
    "      deadline_ms: 7000\n"
    "    tls:\n"
    "      ca_file: identity-ca.pem\n";

static const char valid_turbodb_config[] = "version: 1\n"
                                           "listener:\n"
                                           "  tls:\n"
                                           "    cert_file: cert.pem\n"
                                           "    key_file: key.pem\n"
                                           "storage:\n"
                                           "  turbodb:\n"
                                           "    driver: sqlite\n"
                                           "    options:\n"
                                           "      filename: control.db\n"
                                           "management:\n"
                                           "  session:\n"
                                           "    capacity: 1024\n"
                                           "    ttl_seconds: 3600\n";

static const char legacy_bootstrap_config[] = "version: 1\n"
                                              "listener:\n"
                                              "  tls:\n"
                                              "    cert_file: cert.pem\n"
                                              "    key_file: key.pem\n"
                                              "storage:\n"
                                              "  turbodb:\n"
                                              "    driver: sqlite\n"
                                              "    options:\n"
                                              "      filename: control.db\n"
                                              "bootstrap:\n"
                                              "  username: admin\n"
                                              "  password_ref: env://FLOWIE_BOOTSTRAP_PASSWORD\n"
                                              "management:\n"
                                              "  session:\n"
                                              "    capacity: 1024\n"
                                              "    ttl_seconds: 3600\n";

static int parse_config(const char *yaml, flowie_control_config_t *config,
                        flowie_control_config_error_t *error) {
  *config = (flowie_control_config_t)FLOWIE_CONTROL_CONFIG_INIT;
  *error = (flowie_control_config_error_t)FLOWIE_CONTROL_CONFIG_ERROR_INIT;
  return flowie_control_config_parse_yaml(yaml, strlen(yaml), config, error);
}

static int parse_session_principal_limit(const char *limit, flowie_control_config_t *config,
                                         flowie_control_config_error_t *error) {
  char yaml[512];
  int size = snprintf(yaml, sizeof(yaml),
                      "version: 1\n"
                      "listener:\n"
                      "  tls:\n"
                      "    cert_file: cert.pem\n"
                      "    key_file: key.pem\n"
                      "storage:\n"
                      "  turbodb:\n"
                      "    driver: sqlite\n"
                      "    options:\n"
                      "      filename: control.db\n"
                      "management:\n"
                      "  session:\n"
                      "    max_sessions_per_principal: %s\n",
                      limit);
  if (size < 0 || (size_t)size >= sizeof(yaml)) return SALTS_ERANGE;
  return parse_config(yaml, config, error);
}

static int parse_listener_stack_size(const char *stack_size, flowie_control_config_t *config,
                                     flowie_control_config_error_t *error) {
  char yaml[512];
  int size = snprintf(yaml, sizeof(yaml),
                      "version: 1\n"
                      "listener:\n"
                      "  coroutine_stack_size: %s\n"
                      "  tls:\n"
                      "    cert_file: cert.pem\n"
                      "    key_file: key.pem\n"
                      "storage:\n"
                      "  turbodb:\n"
                      "    driver: sqlite\n"
                      "    options:\n"
                      "      filename: control.db\n",
                      stack_size);
  if (size <= 0 || (size_t)size >= sizeof(yaml)) return SALTS_ENOMEM;
  return flowie_control_config_parse_yaml(yaml, (size_t)size, config, error);
}

spec("Flowie controller configuration") {
  static flowie_control_config_t config;
  static flowie_control_config_error_t error;

  before_each() {
    config = (flowie_control_config_t)FLOWIE_CONTROL_CONFIG_INIT;
    error = (flowie_control_config_error_t)FLOWIE_CONTROL_CONFIG_ERROR_INIT;
  }

  it("accepts only the single TurboDB storage boundary") {
    static const char yaml[] = "version: 1\n"
                               "listener:\n"
                               "  tls:\n"
                               "    cert_file: cert.pem\n"
                               "    key_file: key.pem\n"
                               "storage:\n"
                               "  turbodb:\n"
                               "    driver: sqlite\n"
                               "    options:\n"
                               "      filename: control.db\n"
                               "      busy_timeout_ms: '1000'\n"
                               "management:\n"
                               "  session:\n"
                               "    capacity: 1024\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_OK);
    check_equal(config.turbodb.driver, "sqlite");
    check_equal(config.turbodb.option_count, 2u);
    check_equal(config.turbodb.options[0].keyword, "filename");
    check_equal(config.turbodb.options[0].value, "control.db");
    check_equal(config.turbodb.options[1].keyword, "busy_timeout_ms");
    check_equal(config.turbodb.options[1].value, "1000");
  }

  it("rejects the removed backend-specific storage format") {
    static const char yaml[] = "version: 1\n"
                               "listener:\n"
                               "  tls:\n"
                               "    cert_file: cert.pem\n"
                               "    key_file: key.pem\n"
                               "storage:\n"
                               "  sqlite:\n"
                               "    path: control.db\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.storage.sqlite");
  }

  it("requires environment references for TurboDB passwords") {
    static const char literal[] = "version: 1\n"
                                  "listener:\n"
                                  "  tls:\n"
                                  "    cert_file: cert.pem\n"
                                  "    key_file: key.pem\n"
                                  "storage:\n"
                                  "  turbodb:\n"
                                  "    driver: postgres\n"
                                  "    options:\n"
                                  "      password: literal-secret\n"
                                  "management: {}\n";
    static const char reference[] = "version: 1\n"
                                    "listener:\n"
                                    "  tls:\n"
                                    "    cert_file: cert.pem\n"
                                    "    key_file: key.pem\n"
                                    "storage:\n"
                                    "  turbodb:\n"
                                    "    driver: postgres\n"
                                    "    options:\n"
                                    "      password: env://FLOWIE_CONTROL_DB_PASSWORD\n"
                                    "management: {}\n";

    check_equal(parse_config(literal, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.storage.turbodb.options.password");
    check_equal(parse_config(reference, &config, &error), SALTS_OK);
    check_equal(config.turbodb.options[0].value, "env://FLOWIE_CONTROL_DB_PASSWORD");
  }

  it("rejects literals and malformed references for sensitive TurboDB options") {
    static const char literal_sslpassword[] =
        "version: 1\nlistener: {tls: {cert_file: cert.pem, key_file: key.pem}}\n"
        "storage: {turbodb: {driver: postgres, options: {sslpassword: literal}}}\n";
    static const char literal_conninfo[] =
        "version: 1\nlistener: {tls: {cert_file: cert.pem, key_file: key.pem}}\n"
        "storage: {turbodb: {driver: postgres, options: {conninfo: 'password=literal'}}}\n";
    static const char malformed_reference[] =
        "version: 1\nlistener: {tls: {cert_file: cert.pem, key_file: key.pem}}\n"
        "storage: {turbodb: {driver: postgres, options: {password: 'env://BAD-NAME'}}}\n";
    static const char valid_references[] =
        "version: 1\nlistener: {tls: {cert_file: cert.pem, key_file: key.pem}}\n"
        "storage: {turbodb: {driver: postgres, options: {"
        "password: 'env://FLOWIE_DB_PASSWORD', "
        "sslpassword: 'env://FLOWIE_DB_SSL_PASSWORD', "
        "conninfo: 'env://FLOWIE_DB_CONNINFO'}}}\nmanagement: {}\n";

    check_equal(parse_config(literal_sslpassword, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.storage.turbodb.options.sslpassword");
    check_equal(parse_config(literal_conninfo, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.storage.turbodb.options.conninfo");
    check_equal(parse_config(malformed_reference, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.storage.turbodb.options.password");
    check_equal(parse_config(valid_references, &config, &error), SALTS_OK);
    check_equal(config.turbodb.option_count, 3u);
  }

#ifdef FLOWIE_CONTROL_TEST_CONFIG_PATH
  it("keeps the shipped controller example inside the schema") {
    check_equal(flowie_control_config_load(FLOWIE_CONTROL_TEST_CONFIG_PATH, &config, &error),
                SALTS_OK);
    check_equal(config.listener.host, "127.0.0.1");
    check_equal(config.listener.coroutine_stack_size,
                FLOWIE_CONTROL_CONFIG_LISTENER_DEFAULT_COROUTINE_STACK_SIZE);
    check_equal(config.management.session_capacity, 1024u);
    check_equal(config.management.session_max_sessions_per_principal, 5u);
  }
#endif

  it("loads a valid configuration with secure defaults") {
    check_equal(parse_config(valid_config, &config, &error), SALTS_OK);
    check_equal(config.listener.host, "127.0.0.1");
    check_equal(config.listener.port, 8443);
    check_equal(config.listener.coroutine_stack_size,
                FLOWIE_CONTROL_CONFIG_LISTENER_DEFAULT_COROUTINE_STACK_SIZE);
    check_equal(config.management.rpc_path, "/v2/control/rpc");
    check_equal(config.management.session_capacity, 512u);
    check_equal(config.management.session_max_sessions_per_principal, 7u);
    check_equal(config.management.session_ttl_seconds, 1800u);
    check_true(config.management.login_executor_configured);
    check_equal(config.management.login_executor_workers, 3u);
    check_equal(config.management.login_executor_queue_capacity, 64u);
    check_equal(config.management.login_executor_deadline_ms, 8000u);
    check_true(config.dashboard_enabled);
    check_false(config.auth.enabled);
    check_equal(config.turbodb.driver, "sqlite");
    check_false(config.auth.local_executor.configured);
    check_equal(config.auth.local_executor.workers, 4u);
    check_equal(config.auth.local_executor.queue_capacity, 128u);
    check_equal(config.auth.local_executor.deadline_ms, 10000u);
    check_equal(config.bootstrap.domain_id, "system");
    check_equal(config.bootstrap.principal_id, "admin");
    check_equal(config.bootstrap.principal_type, "human");
    check_equal(FLOWIE_CONTROL_SYSTEM_ADMIN_INITIAL_PASSWORD, "Flowie@ChangeMe!");
  }

  it("defaults each principal to five concurrent management sessions") {
    check_equal(parse_config(valid_turbodb_config, &config, &error), SALTS_OK);
    check_equal(config.management.session_max_sessions_per_principal, 5u);
  }

  it("rejects a listener coroutine stack below the safe minimum") {
    check_equal(parse_listener_stack_size("262143", &config, &error), SALTS_ERANGE);
    check_equal(error.path, "$.listener.coroutine_stack_size");
  }

  it("rejects a listener coroutine stack above the supported maximum") {
    check_equal(parse_listener_stack_size("2097153", &config, &error), SALTS_ERANGE);
    check_equal(error.path, "$.listener.coroutine_stack_size");
  }

  it("rejects a zero per-principal management session limit") {
    check_equal(parse_session_principal_limit("0", &config, &error), SALTS_ERANGE);
    check_equal(error.path, "$.management.session.max_sessions_per_principal");
  }

  it("rejects a per-principal management session limit above 65536") {
    check_equal(parse_session_principal_limit("65537", &config, &error), SALTS_ERANGE);
    check_equal(error.path, "$.management.session.max_sessions_per_principal");
  }

  it("rejects the legacy configurable bootstrap block") {
    check_equal(parse_config(legacy_bootstrap_config, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.bootstrap");
  }

  it("rejects multiple removed backend-specific storage blocks") {
    static const char yaml[] =
        "version: 1\n"
        "listener:\n"
        "  tls:\n"
        "    cert_file: cert.pem\n"
        "    key_file: key.pem\n"
        "storage:\n"
        "  control_store: postgresql\n"
        "  sqlite:\n"
        "    path: control.db\n"
        "  postgresql:\n"
        "    conninfo: host=db.internal dbname=flowie user=flowie sslmode=verify-full\n"
        "    password_ref: env://FLOWIE_CONTROL_PG_PASSWORD\n"
        "management:\n"
        "  session:\n"
        "    capacity: 1024\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.storage.control_store");
  }

  it("rejects unknown fields") {
    static const char yaml[] = "version: 1\n"
                               "unexpected: true\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.unexpected");
  }

  it("rejects duplicate mapping fields") {
    static const char yaml[] = "version: 1\n"
                               "version: 1\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$");
  }

  it("rejects literal TLS key passwords") {
    char yaml[sizeof(valid_config) + 32u];
    const char *reference = "env://FLOWIE_CONTROL_KEY_PASSWORD";
    char *position;

    memcpy(yaml, valid_config, sizeof(valid_config));
    position = strstr(yaml, reference);
    check_not_null(position);
    memcpy(position, "literal-secret", sizeof("literal-secret") - 1u);
    memmove(position + sizeof("literal-secret") - 1u, position + strlen(reference),
            strlen(position + strlen(reference)) + 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.listener.tls.key_password_ref");
  }

  it("rejects the removed static service binding configuration") {
    static const char yaml[] = "version: 1\n"
                               "listener:\n"
                               "  tls:\n"
                               "    cert_file: cert.pem\n"
                               "    key_file: key.pem\n"
                               "storage:\n"
                               "  turbodb:\n"
                               "    driver: sqlite\n"
                               "    options:\n"
                               "      filename: control.db\n"
                               "management:\n"
                               "  session:\n"
                               "    capacity: 1024\n"
                               "auth:\n"
                               "  enabled: true\n"
                               "  listener_id: auth-listener\n"
                               "  method: password\n"
                               "  service_bindings:\n"
                               "    - service_id: broker-main\n"
                               "      token_ref: env://FLOWIE_BROKER_A_TOKEN\n"
                               "      domain: root-a\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.auth.service_bindings");
  }

  it("requires the listener body limit to cover the RPC limit") {
    static const char yaml[] = "version: 1\n"
                               "listener:\n"
                               "  tls:\n"
                               "    cert_file: cert.pem\n"
                               "    key_file: key.pem\n"
                               "  limits:\n"
                               "    max_request_body_size: 4096\n"
                               "storage:\n"
                               "  turbodb:\n"
                               "    driver: sqlite\n"
                               "    options:\n"
                               "      filename: control.db\n"
                               "management:\n"
                               "  rpc_max_request_size: 8192\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_ERANGE);
    check_equal(error.path, "$.listener.limits.max_request_body_size");
  }

  it("selects local authentication when no external HTTPS source is configured") {
    static const char yaml[] = "version: 1\n"
                               "listener:\n"
                               "  tls:\n"
                               "    cert_file: cert.pem\n"
                               "    key_file: key.pem\n"
                               "storage:\n"
                               "  turbodb:\n"
                               "    driver: sqlite\n"
                               "    options:\n"
                               "      filename: control.db\n"
                               "management:\n"
                               "  session:\n"
                               "    capacity: 1024\n"
                               "auth:\n"
                               "  enabled: true\n"
                               "  listener_id: flowie-control-auth\n"
                               "  method: password\n"
                               "  local_executor:\n"
                               "    workers: 6\n"
                               "    queue_capacity: 256\n"
                               "    deadline_ms: 12000\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_OK);
    check_true(config.auth.enabled);
    check_false(config.auth.external_https.enabled);
    check_equal(config.auth.method, "password");
    check_true(config.auth.local_executor.configured);
    check_equal(config.auth.local_executor.workers, 6u);
    check_equal(config.auth.local_executor.queue_capacity, 256u);
    check_equal(config.auth.local_executor.deadline_ms, 12000u);
  }

  it("rejects local executor configuration with external HTTPS authentication") {
    char yaml[sizeof(valid_external_https_config) + 96u];
    char *external;
    static const char executor[] = "  local_executor:\n"
                                   "    workers: 2\n"
                                   "    queue_capacity: 16\n"
                                   "    deadline_ms: 5000\n";

    memcpy(yaml, valid_external_https_config, sizeof(valid_external_https_config));
    external = strstr(yaml, "  external_https:\n");
    check_not_null(external);
    memmove(external + sizeof(executor) - 1u, external, strlen(external) + 1u);
    memcpy(external, executor, sizeof(executor) - 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.auth.local_executor");
  }

  it("rejects a management login executor with external HTTPS authentication") {
    char yaml[sizeof(valid_external_https_config) + 128u];
    char *session;
    static const char executor[] = "  login_executor:\n"
                                   "    workers: 2\n"
                                   "    queue_capacity: 16\n"
                                   "    deadline_ms: 5000\n";

    memcpy(yaml, valid_external_https_config, sizeof(valid_external_https_config));
    session = strstr(yaml, "  session:\n");
    check_not_null(session);
    memmove(session + sizeof(executor) - 1u, session, strlen(session) + 1u);
    memcpy(session, executor, sizeof(executor) - 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.management.login_executor");
  }

  it("rejects a local executor deadline above the hard bound") {
    static const char yaml[] = "version: 1\n"
                               "listener:\n"
                               "  tls:\n"
                               "    cert_file: cert.pem\n"
                               "    key_file: key.pem\n"
                               "storage:\n"
                               "  turbodb:\n"
                               "    driver: sqlite\n"
                               "    options:\n"
                               "      filename: control.db\n"
                               "management:\n"
                               "  session:\n"
                               "    capacity: 1024\n"
                               "auth:\n"
                               "  enabled: true\n"
                               "  listener_id: flowie-control-auth\n"
                               "  method: password\n"
                               "  local_executor:\n"
                               "    deadline_ms: 60001\n";
    check_equal(parse_config(yaml, &config, &error), SALTS_ERANGE);
    check_equal(error.path, "$.auth.local_executor.deadline_ms");
  }

  it("loads the bounded external HTTPS authentication configuration") {
    check_equal(parse_config(valid_external_https_config, &config, &error), SALTS_OK);
    check_true(config.auth.external_https.enabled);
    check_equal(config.auth.external_https.url, "https://auth.example/v1/assert");
    check_equal(config.auth.external_https.service_token_ref,
                "env://FLOWIE_THIRD_PARTY_AUTH_TOKEN");
    check_equal(config.auth.external_https.trusted_issuer, "https://identity.example");
    check_equal(config.auth.external_https.subject_type, "device");
    check_equal(config.auth.external_https.timeout_ms, 2500u);
    check_equal(config.auth.external_https.max_response_size, 8192u);
    check_equal(config.auth.external_https.max_in_flight, 32u);
    check_equal(config.auth.external_https.tls.client_cert_file, "flowie-client.pem");
  }

  it("rejects an external HTTPS concurrency limit above the hard bound") {
    char yaml[sizeof(valid_external_https_config) + 2u];
    char *limit;

    memcpy(yaml, valid_external_https_config, sizeof(valid_external_https_config));
    limit = strstr(yaml, "max_in_flight: 32");
    check_not_null(limit);
    limit += sizeof("max_in_flight: ") - 1u;
    memmove(limit + sizeof("2048") - 1u, limit + sizeof("32") - 1u,
            strlen(limit + sizeof("32") - 1u) + 1u);
    memcpy(limit, "2048", sizeof("2048") - 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_ERANGE);
    check_equal(error.path, "$.auth.external_https.max_in_flight");
  }

  it("rejects insecure external authentication URLs") {
    char yaml[sizeof(valid_external_https_config)];
    char *scheme;

    memcpy(yaml, valid_external_https_config, sizeof(valid_external_https_config));
    scheme = strstr(yaml, "https://auth.example");
    check_not_null(scheme);
    memmove(scheme + sizeof("http://") - 1u, scheme + sizeof("https://") - 1u,
            strlen(scheme + sizeof("https://") - 1u) + 1u);
    memcpy(scheme, "http://", sizeof("http://") - 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.auth.external_https.url");
  }

  it("requires a complete external mTLS client identity") {
    char yaml[sizeof(valid_external_https_config)];
    char *key_line;
    char *line_end;

    memcpy(yaml, valid_external_https_config, sizeof(valid_external_https_config));
    key_line = strstr(yaml, "      client_key_file:");
    check_not_null(key_line);
    line_end = strchr(key_line, '\n');
    check_not_null(line_end);
    memmove(key_line, line_end + 1u, strlen(line_end + 1u) + 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.auth.external_https.tls");
  }

  it("rejects external authentication while the auth endpoint is disabled") {
    char yaml[sizeof(valid_external_https_config) + 1u];
    char *enabled;

    memcpy(yaml, valid_external_https_config, sizeof(valid_external_https_config));
    enabled = strstr(yaml, "  enabled: true");
    check_not_null(enabled);
    enabled += sizeof("  enabled: ") - 1u;
    memmove(enabled + sizeof("false") - 1u, enabled + sizeof("true") - 1u,
            strlen(enabled + sizeof("true") - 1u) + 1u);
    memcpy(enabled, "false", sizeof("false") - 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.auth.external_https");
  }

  it("loads bounded JWT JWKS authentication configuration") {
    check_equal(parse_config(valid_jwt_jwks_config, &config, &error), SALTS_OK);
    check_true(config.auth.jwt_jwks.enabled);
    check_false(config.auth.external_https.enabled);
    check_equal(config.auth.jwt_jwks.url, "https://identity.example/.well-known/jwks.json");
    check_equal(config.auth.jwt_jwks.trusted_issuer, "https://identity.example");
    check_equal(config.auth.jwt_jwks.audience, "flowie");
    check_equal(config.auth.jwt_jwks.subject_type, "device");
    check_equal(config.auth.jwt_jwks.algorithm, "EdDSA");
    check_equal(config.auth.jwt_jwks.timeout_ms, 2400u);
    check_equal(config.auth.jwt_jwks.max_response_size, 32768u);
    check_equal(config.auth.jwt_jwks.max_keys, 8u);
    check_equal(config.auth.jwt_jwks.max_token_size, 3072u);
    check_equal(config.auth.jwt_jwks.refresh_interval_seconds, 120u);
    check_equal(config.auth.jwt_jwks.clock_skew_seconds, 15u);
    check_equal(config.auth.jwt_jwks.executor_workers, 3u);
    check_equal(config.auth.jwt_jwks.executor_queue_capacity, 48u);
    check_equal(config.auth.jwt_jwks.executor_deadline_ms, 7000u);
    check_equal(config.auth.jwt_jwks.ca_file, "identity-ca.pem");
  }

  it("rejects symmetric JWT algorithms") {
    char yaml[sizeof(valid_jwt_jwks_config)];
    char *algorithm;

    memcpy(yaml, valid_jwt_jwks_config, sizeof(valid_jwt_jwks_config));
    algorithm = strstr(yaml, "algorithm: EdDSA");
    check_not_null(algorithm);
    memcpy(algorithm + sizeof("algorithm: ") - 1u, "HS256", sizeof("HS256") - 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.auth.jwt_jwks.algorithm");
  }

  it("rejects simultaneous external authentication providers") {
    char yaml[sizeof(valid_jwt_jwks_config) + sizeof(valid_external_https_config)];
    const char *external = strstr(valid_external_https_config, "  external_https:\n");

    check_not_null(external);
    memcpy(yaml, valid_jwt_jwks_config, sizeof(valid_jwt_jwks_config));
    memcpy(yaml + sizeof(valid_jwt_jwks_config) - 1u, external, strlen(external) + 1u);
    check_equal(parse_config(yaml, &config, &error), SALTS_EINVAL);
    check_equal(error.path, "$.auth.jwt_jwks");
  }
}
