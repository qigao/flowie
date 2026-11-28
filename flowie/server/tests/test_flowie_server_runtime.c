#include <chttp_app/application.h>

#include "flowie_server_runtime_internal.h"
#include "flowie_server_turbodb_config_internal.h"
#include "flowie_rulesforge_package_internal.h"
#include "flowie_control_test_turbodb.h"
#include "flowie_release_network_fixture.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include <core/error_codes.h>
#include <crypto/sm2_x509.h>
#include <file/fs.h>
#include <json_parser.h>

#include <stdint.h>
#include <stdatomic.h>
#include <threads/threads.h>
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

static const char server_test_praktor_yaml[] =
    "name: Flowie handoff\ntasks:\n  - name: verify\n    command: echo practical-yaml\n";
static const uint8_t server_test_wasm[] = {
    0, 'a', 's', 'm', 1, 0, 0, 0, 1, 5, 1, 0x60, 0, 1, 0x7f,
    3, 2, 1, 0, 7, 5, 1, 1, 'f', 0, 0, 10, 6, 1, 4, 0, 0x41, 42, 0x0b};

typedef struct server_test_identity_s {
  const char *peer_pin;
  int port;
  atomic_uint calls;
  atomic_uint mode;
  chttp_application app;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
} server_test_identity_t;

cmeta_reflect_data(server_test_identity_t, "flowie.test.Identity", cmeta_field(int, port));
cmeta_component_empty(FlowieTestIdentity);

static int server_test_identity_token(const char *value) {
#ifdef _WIN32
  return _putenv_s("FLOWIE_TEST_RELEASE_AUTH_TOKEN", value ? value : "");
#else
  return value ? setenv("FLOWIE_TEST_RELEASE_AUTH_TOKEN", value, 1) :
                 unsetenv("FLOWIE_TEST_RELEASE_AUTH_TOKEN");
#endif
}

static cmeta_status server_test_identity_create(void *context, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *dependencies, size_t count,
    cmeta_object_ref *out) {
  (void)data;
  (void)config;
  (void)dependencies;
  if (count) return CMETA_INVALID_ARGUMENT;
  return cmeta_object_borrow(out, context, cmeta_reflected_data(server_test_identity_t), NULL);
}

static int server_test_identity_field(const json_value_t *body, const char *key, const char *expected) {
  const json_value_t *field = json_object_get(body, key);
  return field && json_type(field) == JSON_STRING && strcmp(json_string(field), expected) == 0;
}

CHTTP_ACTION(server_test_identity_authenticate);
int server_test_identity_authenticate(void *self, const chttp_invocation *invocation) {
  server_test_identity_t *auth = self;
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  atomic_fetch_add(&auth->calls, 1u);
  const char *token = chttp_server_request_header(request, "Authorization");
  const char *caller = chttp_server_request_header(request, "X-Flowie-Service-Id");
  const char *domain = chttp_server_request_header(request, "X-Flowie-Service-Domain");
  if (!token || strcmp(token, "Bearer test-release-token") || !caller || strcmp(caller, "broker-main") ||
      !domain || strcmp(domain, "platform-services"))
    return chttp_invocation_respond(invocation, 400u, NULL, NULL, 0u);
  json_value_t *body = json_parse(request->body, request->body_size);
  const int valid = body && server_test_identity_field(body, "identity", "registration-42") &&
      server_test_identity_field(body, "method", "password") &&
      server_test_identity_field(body, "protocol", "mqtt5") &&
      server_test_identity_field(body, "peer_certificate_sha256", auth->peer_pin);
  const int verified = valid && server_test_identity_field(body, "secret_base64", "bW90b3I=");
  json_free(body);
  if (!valid) return chttp_invocation_respond(invocation, 400u, NULL, NULL, 0u);
  if (!verified) return chttp_invocation_respond(invocation, 401u, NULL, NULL, 0u);
  const unsigned int mode = atomic_load(&auth->mode);
  if (mode == 2u) return chttp_invocation_respond(invocation, 503u, NULL, NULL, 0u);
  char response[256];
  const int size = snprintf(response, sizeof(response),
      "{\"version\":3,\"authenticated\":true,\"principal\":{\"id\":\"writer\",\"type\":\"device\","
      "\"domain\":\"%s\",\"auth_method\":\"password\",\"scope\":\"self\",\"roles\":[],"
      "\"groups\":[],\"expires_at\":0,\"policy_version\":42}}", mode ? "elsewhere" : "device-a");
  return chttp_invocation_respond(invocation, 200u, "application/json", response, (size_t)size);
}

CHTTP_ACTION(server_test_package_authorize);
int server_test_package_authorize(void *self, const chttp_invocation *invocation) {
  server_test_identity_t *auth = self;
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  const char *token = chttp_server_request_header(request, "Authorization");
  const char *target = chttp_server_request_header(request, FLOWIE_RELEASE_ACCESS_DOMAIN);
  const char *action = chttp_server_request_header(request, FLOWIE_RELEASE_ACCESS_ACTION);
  if (!target || strcmp(target, "device-a") || !action || request->body_size)
    return chttp_invocation_respond(invocation, 400u, NULL, NULL, 0u);
  if (!token) return chttp_invocation_respond(invocation, 401u, NULL, NULL, 0u);
  const char *actor = NULL;
  if (strcmp(token, "Bearer package-owner") == 0) actor = "8:device-a/release-owner";
  if (strcmp(token, "Bearer package-ci") == 0 && atomic_load(&auth->mode) == 0u &&
      (strcmp(action, FLOWIE_RELEASE_ACCESS_UPLOAD) == 0 || strcmp(action, FLOWIE_RELEASE_ACCESS_VALIDATE) == 0)) {
    const char *id = chttp_server_request_header(request, "X-Flowie-Service-Id");
    const char *domain = chttp_server_request_header(request, "X-Flowie-Service-Domain");
    if (id && strcmp(id, "release-ci") == 0 && domain && strcmp(domain, "device-a") == 0)
      actor = "8:device-a/release-ci";
  }
  if (!actor) return chttp_invocation_respond(invocation, 403u, NULL, NULL, 0u);
  const int rc = chttp_invocation_set_header(invocation, FLOWIE_RELEASE_ACCESS_ACTOR, actor);
  return rc == SALTS_OK ? chttp_invocation_respond(invocation, 204u, NULL, NULL, 0u) : rc;
}

static int server_test_identity_start(server_test_identity_t *auth, const test_release_https_t *https) {
  auth->peer_pin = https->mqtt_pin;
  atomic_init(&auth->calls, 0u);
  atomic_init(&auth->mode, 0u);
  auth->provider = (salts_configurator_provider_binding){sizeof(auth->provider),
      SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(FlowieTestIdentity),
      auth, NULL, server_test_identity_create, NULL, NULL};
  auth->deployment = (salts_configurator_deployment){&auth->provider, NULL, NULL};
  const cnet_tls_server_config tls = {.size = sizeof(tls),
      .cert_file = https->files[TEST_RELEASE_TLS_SERVER_CERT],
      .key_file = https->files[TEST_RELEASE_TLS_SERVER_KEY]};
  const chttp_action actions[] = {CHTTP_POST("/v4/authenticate", server_test_identity_authenticate),
      CHTTP_POST(FLOWIE_RELEASE_ACCESS_PATH, server_test_package_authorize)};
  const chttp_controller controller = {.component_id = "FlowieTestIdentity",
      .receiver = cmeta_reflected_data(server_test_identity_t), .actions = actions, .action_count = 2u};
  chttp_application_config app = chttp_application_config_default();
  app.http.tls = &tls;
  app.http.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  app.http.network.tls_handshake_timeout_ms = 1000u;
  app.components = &auth->deployment;
  app.component_count = 1u;
  app.dependency_capacity = 1u;
  app.controllers = &controller;
  app.controller_count = 1u;
  int rc = chttp_application_init(&auth->app, &app);
  if (rc == SALTS_OK) rc = chttp_application_start(&auth->app);
  uint16_t port = 0;
  if (rc == SALTS_OK) rc = chttp_application_port(&auth->app, &port);
  auth->port = port;
  return rc;
}

static const char rulesforge_test_rfl[] =
    "import schema \"order.schema\"\n"
    "declare WindowAmount result: double end\n"
    "declare Delivery sink: String, body: String end\n"
    "rule \"Dispatch window amount\"\n"
    "when\n"
    "  $amount: WindowAmount(result > 0) from accumulate(\n"
    "    $order: Order() over window:time(100) from entry-point \"orders\", sum($order.amount))\n"
    "then\n"
    "  call \"pricing.calculate\"($amount.result) into $price\n"
    "  if ($price.total > 30.0) { insert Delivery { sink = \"alerts\", body = \"high\" } }\n"
    "  else { insert Delivery { sink = \"alerts\", body = \"normal\" } }\n"
    "end\n";
static const char rulesforge_test_schema[] =
    "schema ScriptPricing [id(1), version(1), byte_order(little)];\n"
    "message Order { double amount; double total; bool done; }\n"
    "message Price { double total; }\n";
static const char rulesforge_test_toad[] =
    "func calculate(amount) { return map {total: amount + 5}; };\nexport(\"calculate\");\n";
static const char rulesforge_test_acl[] = "user writer allow {\n  readwrite topic device-a/event\n}";

static int rulesforge_test_pack(const char *descriptor, const test_release_asset_t *extra, const char *acl,
    const uint8_t *base,
    const salts_crypto_sm2_cert_span *certificate, const uint8_t *private_key,
    const void **out, size_t *size) {
  const test_release_asset_t assets[] = {
      {"acl.yml", acl, strlen(acl)},
      {FLOWIE_RELEASE_RULESFORGE_PATH, descriptor, strlen(descriptor)},
      {"rules/pricing.rfl", rulesforge_test_rfl, sizeof(rulesforge_test_rfl) - 1u},
      {"rules/order.schema", rulesforge_test_schema, sizeof(rulesforge_test_schema) - 1u},
      {"rules/pricing.toad", rulesforge_test_toad, sizeof(rulesforge_test_toad) - 1u},
      {extra ? extra->path : NULL, extra ? extra->bytes : NULL, extra ? extra->size : 0u}};
  char base_text[2u * FLOWIE_RELEASE_DIGEST_BYTES + 3u] = "null";
  if (base) {
    base_text[0] = '"';
    test_release_hex(base, base_text + 1);
    base_text[sizeof(base_text) - 2u] = '"';
  }
  char text[2048];
  const int head = snprintf(text, sizeof(text),
      "{\"name\":\"orders\",\"domain\":\"device-a\",\"environment\":\"qa\",\"base_release\":%s,"
      "\"entrypoints\":{\"acl\":\"acl.yml\",\"rulesforge\":\"" FLOWIE_RELEASE_RULESFORGE_PATH "\"},\"files\":[",
      base_text);
  if (head < 0 || (size_t)head >= sizeof(text)) return SALTS_ERANGE;
  size_t length = (size_t)head, used = 0;
  static char tar[TEST_TAR_CAPACITY * 2u];
  for (size_t i = 0; i < sizeof(assets) / sizeof(assets[0]) && assets[i].path; ++i) {
    const test_release_asset_t *asset = &assets[i];
    uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
    char hex[FLOWIE_RELEASE_DIGEST_BYTES * 2u + 1u];
    if (salts_crypto_sm3(asset->bytes, asset->size, digest) != SALTS_CRYPTO_OK) return SALTS_EIO;
    test_release_hex(digest, hex);
    const int count = snprintf(text + length, sizeof(text) - length,
        "%s{\"path\":\"%s\",\"size\":%zu,\"sm3\":\"%s\"}", i ? "," : "", asset->path,
        asset->size, hex);
    if (count < 0 || (size_t)count >= sizeof(text) - length - sizeof("]}")) return SALTS_ERANGE;
    length += (size_t)count;
    used = test_tar_add(tar, used, asset->path, NULL, asset->bytes, asset->size);
  }
  memcpy(text + length, "]}", sizeof("]}"));
  static uint8_t signature[SALTS_CRYPTO_SM2_CMS_MAX_DER];
  size_t signature_size = 0;
  if (test_release_sign(private_key, certificate, text, signature, &signature_size)
      != SALTS_CRYPTO_OK)
    return SALTS_EIO;
  used = test_tar_manifest(tar, used, text, signature, signature_size);
  static uint8_t compressed[sizeof(tar) * 2u];
  const int rc = zstd_compress(compressed, sizeof(compressed), size, tar, used, 0);
  if (rc == SALTS_OK) *out = compressed;
  return rc;
}

static int rulesforge_test_archive(const char *descriptor, const char *extra_path,
    const salts_crypto_sm2_cms_verify_config *verify, const uint8_t *private_key,
    flowie_release_archive_t **out) {
  const void *bytes;
  size_t size;
  const test_release_asset_t extra = {extra_path, "native", 6u};
  int rc = rulesforge_test_pack(descriptor, &extra, rulesforge_test_acl, NULL, &verify->chain->certificate,
      private_key, &bytes, &size);
  if (rc == SALTS_OK)
    rc = flowie_control_release_archive_verify(verify, "device-a", "qa", bytes, size, out);
  return rc;
}

static void rulesforge_check_batch(const flowie_rulesforge_batch_t *batch, const char *body) {
  check_equal(batch->output_count, 1u);
  check_equal(batch->outputs[0].sink_size, 6u);
  check_equal(memcmp(batch->outputs[0].sink, "alerts", 6u), 0);
  check_equal(batch->outputs[0].body_size, strlen(body));
  check_equal(memcmp(batch->outputs[0].body, body, strlen(body)), 0);
}

typedef struct rulesforge_test_processing_s {
  flowie_rulesforge_package_config_t config;
  atomic_uint prepare_calls;
  atomic_uint live;
  atomic_uint last_closed;
  atomic_int reject;
} rulesforge_test_processing_t;

typedef struct rulesforge_test_candidate_s {
  flowie_rulesforge_package_t *package;
  unsigned int ordinal;
} rulesforge_test_candidate_t;

static int rulesforge_test_prepare(void *context, const flowie_release_archive_t *archive, void **out) {
  rulesforge_test_processing_t *state = context;
  *out = NULL;
  rulesforge_test_candidate_t *candidate = calloc(1u, sizeof(*candidate));
  if (!candidate) return SALTS_ENOMEM;
  candidate->ordinal = atomic_fetch_add(&state->prepare_calls, 1u) + 1u;
  const int rc = state->config.provider ?
      flowie_rulesforge_package_prepare(&state->config, archive, &candidate->package) : SALTS_OK;
  if (rc != SALTS_OK) {
    free(candidate);
    return rc;
  }
  *out = candidate;
  atomic_fetch_add(&state->live, 1u);
  return atomic_load(&state->reject) ? SALTS_EBUSY : SALTS_OK;
}

static int rulesforge_test_close(void *context, void *handle) {
  rulesforge_test_processing_t *state = context;
  rulesforge_test_candidate_t *candidate = handle;
  const int rc = flowie_rulesforge_package_close(&candidate->package);
  atomic_store(&state->last_closed, candidate->ordinal);
  free(candidate);
  atomic_fetch_sub(&state->live, 1u);
  return rc;
}

static int rulesforge_test_auth(void *context, const flowie_security_auth_request_t *request,
    flowie_security_principal_t *out) {
  (void)context;
  (void)request;
  *out = (flowie_security_principal_t)FLOWIE_SECURITY_PRINCIPAL_INIT;
  strcpy(out->principal_id, "writer");
  strcpy(out->principal_type, "device");
  strcpy(out->domain_id, "device-a");
  strcpy(out->auth_method, "mtls");
  out->scope = FLOWIE_SECURITY_SCOPE_SELF;
  out->policy_version = 1u;
  return SALTS_OK;
}

static void rulesforge_test_check_acl(flowie_control_release_mqtt_t *mqtt, uint64_t version,
    int expected) {
  const flowie_endpoint_security_binding_t *binding = flowie_control_release_mqtt_binding(mqtt);
  flowie_security_auth_request_t auth = FLOWIE_SECURITY_AUTH_REQUEST_INIT;
  auth.identity = "writer";
  auth.method = "mtls";
  flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
  check_equal(flowie_security_authenticate(binding->auth_provider, &auth, &principal), SALTS_OK);
  check_equal(principal.policy_version, version);
  flowie_security_request_t request = FLOWIE_SECURITY_REQUEST_INIT;
  request.principal = &principal;
  request.domain_id = "device-a";
  request.action = FLOWIE_SECURITY_ACTION_PUBLISH;
  request.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
  request.resource = "device-a/event";
  flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;
  check_equal(flowie_security_realm_authorize(binding->realm, &request, (uint64_t)time(NULL),
      &decision), expected);
}

static void rulesforge_test_upload(test_release_https_t *https, const void *bytes, size_t size,
    uint8_t *digest, char *validate, size_t validate_capacity, char *body, size_t body_capacity) {
  static const chttp_header headers[] = {
      {"Content-Type", "application/zstd"}, {"Flowie-Release-Signer", "signer-a"}};
  check_equal(test_release_https_request(https, CHTTP_METHOD_POST, "/domains/device-a/releases",
      headers, 2u, bytes, size, 0), SALTS_OK);
  check_equal(https->response.status_code, 201u);
  check_equal(salts_crypto_sha256(bytes, size, digest), SALTS_CRYPTO_OK);
  char hex[2u * FLOWIE_RELEASE_DIGEST_BYTES + 1u];
  test_release_hex(digest, hex);
  snprintf(validate, validate_capacity, "/domains/device-a/releases/%s/validate", hex);
  snprintf(body, body_capacity, "{\"digest\":\"%s\"}", hex);
}

spec("Flowie signed RulesForge package preparation") {
  static X509_KEY *ca_key, *leaf_key;
  static tls_test_certificate *ca, *leaf;
  static char *staging_root;
  static char *release_database;
  static flowie_release_archive_t *archive;
  static flowie_rulesforge_package_t *package;
  static flowie_control_release_store_t *store;
  static flowie_control_release_mqtt_t *mqtt;
  static test_release_https_t https;
  static rulesforge_test_processing_t processing_state;
  static flowie_control_release_processing_t processing;
  static uint8_t private_key[SALTS_CRYPTO_SM2_PRIVATE_KEY_SIZE];
  static uint8_t pin[SALTS_CRYPTO_SM2_PUBLIC_KEY_SIZE];
  static salts_crypto_sm2_chain_entry chain;
  static salts_crypto_sm2_trust_policy trust;
  static salts_crypto_sm2_cms_verify_config verify;

  before_each() {
    ca_key = test_release_generate_key();
    leaf_key = test_release_generate_key();
    check_not_null(ca_key);
    check_not_null(leaf_key);
    ca = test_release_certificate(ca_key, NULL, NULL, 0);
    check_not_null(ca);
    leaf = test_release_certificate(leaf_key, ca, ca_key, OID_kp_code_signing);
    check_not_null(leaf);
    sm2_z256_to_bytes(leaf_key->u.sm2_key.private_key, private_key);
    check_equal(salts_crypto_sm2_x509_public_key(leaf->der, leaf->size, pin), SALTS_CRYPTO_OK);
    chain = (salts_crypto_sm2_chain_entry){{leaf->der, leaf->size}, issuer_id, sizeof(issuer_id) - 1u};
    trust = (salts_crypto_sm2_trust_policy){.anchor = {ca->der, ca->size},
        .expected_leaf_public_key = pin, .validation_time = (int64_t)time(NULL),
        .purpose = SALTS_CRYPTO_SM2_TRUST_CODE_SIGNING,
        .revocation = SALTS_CRYPTO_SM2_REVOCATION_NOT_CHECKED};
    verify = (salts_crypto_sm2_cms_verify_config){.format = SALTS_CRYPTO_SM2_CMS_RFC5652,
        .detached = true, .signer_identity = signer_id, .signer_identity_size = sizeof(signer_id) - 1u,
        .chain = &chain, .chain_count = 1u, .trust = &trust};
    staging_root = tt_make_temp_dir("flowie-rulesforge");
    check_not_null(staging_root);
  }

  after_each() {
    test_release_https_close(&https);
    flowie_control_release_mqtt_close(mqtt);
    mqtt = NULL;
    flowie_control_release_store_close(store);
    store = NULL;
    if (release_database) remove(release_database);
    free(release_database);
    release_database = NULL;
    (void)flowie_rulesforge_package_close(&package);
    flowie_control_release_archive_destroy(archive);
    archive = NULL;
    const int removed = staging_root ? fs_rmdir(staging_root) : SALTS_OK;
    free(staging_root);
    staging_root = NULL;
    free(leaf);
    leaf = NULL;
    free(ca);
    ca = NULL;
    tls_test_pki_free_key(leaf_key);
    leaf_key = NULL;
    tls_test_pki_free_key(ca_key);
    ca_key = NULL;
    gmssl_secure_clear(private_key, sizeof(private_key));
    check_equal(removed, SALTS_OK);
  }

#ifdef _WIN32
#define TEST_RULESFORGE_PROVIDER "librulesforge_toad.dll"
#elif defined(__APPLE__)
#define TEST_RULESFORGE_PROVIDER "librulesforge_toad.dylib"
#else
#define TEST_RULESFORGE_PROVIDER "librulesforge_toad.so"
#endif
#define TEST_RULESFORGE_PROGRAM_FIELDS \
    "\"rules\":\"pricing.rfl\",\"signatures\":\"order.schema\",\"mode\":\"interpreter\"," \
    "\"modules\":[{\"name\":\"pricing\",\"path\":\"pricing.toad\"}]," \
    "\"bindings\":[{\"name\":\"pricing.calculate\",\"module\":\"pricing\",\"function\":\"calculate\"," \
    "\"parameters\":[\"double\"],\"result\":\"Price\"}]," \
    "\"limits\":{\"source_bytes\":8192,\"value_bytes\":8192,\"value_nodes\":1024,\"value_depth\":16," \
    "\"steps\":10000,\"loops\":1000,\"recursion\":32,\"retained_bytes\":1048576," \
    "\"stack_bytes\":65536,\"max_sessions\":1}"
#define TEST_RULESFORGE_PROGRAM "{\"provider\":\"" TEST_RULESFORGE_PROVIDER "\"," \
    TEST_RULESFORGE_PROGRAM_FIELDS "}"

  it("rejects descriptor-controlled native loads and executable assets before staging") {
    static const struct { const char *descriptor; const char *extra_path; } cases[] = {
        {"{\"provider\":\"" TEST_RULESFORGE_PROVIDER "\",\"native\\u005fmodules\":[],"
         TEST_RULESFORGE_PROGRAM_FIELDS "}", NULL},
        {"{\"provider\":\"external.so\"," TEST_RULESFORGE_PROGRAM_FIELDS "}", NULL},
        {"{\"provider\":\"" TEST_RULESFORGE_PROVIDER "\",\"prov\\u0069der\":\"external.so\","
         TEST_RULESFORGE_PROGRAM_FIELDS "}", NULL},
        {"{\"provider\":\"" TEST_RULESFORGE_PROVIDER "\",\"mode\":\"interpreter\","
        "\"rules\":\"pricing.rfl\",\"signatures\":\"order.schema\",\"modules\":["
        "{\"name\":\"pricing\",\"path\":\"" TEST_RULESFORGE_PROVIDER "\",\"path\":\"pricing.toad\"}]}", NULL},
        {TEST_RULESFORGE_PROGRAM, "rules/" TEST_RULESFORGE_PROVIDER}};
    char missing[4096], provider[4096];
    check_equal(fs_path_join(missing, sizeof(missing), staging_root, "missing-worker"), SALTS_OK);
    check_equal(fs_path_join(provider, sizeof(provider), staging_root, TEST_RULESFORGE_PROVIDER), SALTS_OK);
    const flowie_rulesforge_package_config_t config = {missing, provider, staging_root, 4000u};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      check_equal(rulesforge_test_archive(cases[i].descriptor, cases[i].extra_path,
          &verify, private_key, &archive), SALTS_OK);
      check_equal(flowie_rulesforge_package_prepare(&config, archive, &package), SALTS_EPROTO);
      check_null(package);
      flowie_control_release_archive_destroy(archive);
      archive = NULL;
    }
  }

  it("stages verified rule assets and releases the private directory on failure or retirement") {
    const char *worker_path = getenv("FLOWIE_TEST_RULESFORGE_EXECUTABLE");
    const char *provider_path = getenv("FLOWIE_TEST_RULESFORGE_PROVIDER");
    char missing[4096], provider[4096];
    check_equal(fs_path_join(missing, sizeof(missing), staging_root, "missing-worker"), SALTS_OK);
    check_equal(fs_path_join(provider, sizeof(provider), staging_root, TEST_RULESFORGE_PROVIDER), SALTS_OK);
    const flowie_rulesforge_package_config_t missing_config = {missing, provider, staging_root, 4000u};
    check_equal(rulesforge_test_archive(TEST_RULESFORGE_PROGRAM, NULL, &verify, private_key,
        &archive), SALTS_OK);
    check_equal(flowie_rulesforge_package_prepare(&missing_config, archive, &package), -ENOENT);
    check_null(package);
    static const char fixture_provider[] = "fixture provider";
    check_equal(tt_write_file(provider, fixture_provider, sizeof(fixture_provider) - 1u), 0);
    const int wrong_worker = flowie_rulesforge_package_prepare(&missing_config, archive, &package);
    check_equal(fs_unlink(provider), SALTS_OK);
    check_true(wrong_worker < 0);
    check_null(package);

    if (provider_path) {
      check_not_null(worker_path);
      const flowie_rulesforge_package_config_t config = {worker_path, provider_path, staging_root, 4000u};
      check_equal(flowie_rulesforge_package_prepare(&config, archive, &package), SALTS_OK);
      flowie_control_release_archive_destroy(archive);
      archive = NULL;
      flowie_rulesforge_worker_t *worker = flowie_rulesforge_package_worker(package);
      check_not_null(worker);
      static const char event[] = "{\"amount\":15,\"total\":0,\"done\":false}";
      flowie_rulesforge_batch_t batch = {0};
      check_equal(flowie_rulesforge_worker_push(worker, 200, "later", "orders", "Order", event,
          sizeof(event) - 1u, &batch), SALTS_OK);
      rulesforge_check_batch(&batch, "normal");
      check_equal(flowie_rulesforge_worker_ack(worker, batch.id), SALTS_OK);
      check_equal(flowie_rulesforge_worker_push(worker, 100, "earlier", "orders", "Order", event,
          sizeof(event) - 1u, &batch), SALTS_OK);
      rulesforge_check_batch(&batch, "high");
      check_equal(flowie_rulesforge_worker_ack(worker, batch.id), SALTS_OK);
      check_equal(flowie_rulesforge_worker_watermark(worker, 201, &batch), SALTS_OK);
      check_equal(batch.expired, 1u);
      rulesforge_check_batch(&batch, "normal");
      check_equal(flowie_rulesforge_worker_ack(worker, batch.id), SALTS_OK);
      check_equal(flowie_rulesforge_worker_drain(worker, &batch), SALTS_OK);
      check_equal(batch.output_count, 0u);
      check_equal(flowie_rulesforge_worker_ack(worker, batch.id), SALTS_OK);
      check_equal(flowie_rulesforge_package_close(&package), SALTS_OK);
    }
  }

  it("publishes RulesForge and ACL together and preserves the live release on preparation failure") {
    static const flowie_security_auth_provider_t identity = {
        sizeof(identity), NULL, rulesforge_test_auth};
    static const flowie_control_release_store_limits_t limits = {4, FLOWIE_RELEASE_MAX_COMPRESSED_BYTES,
        4, FLOWIE_RELEASE_MAX_COMPRESSED_BYTES};
    const char *worker = getenv("FLOWIE_TEST_RULESFORGE_EXECUTABLE");
    const char *provider = getenv("FLOWIE_TEST_RULESFORGE_PROVIDER");
    if (provider) check_not_null(worker);
    processing_state.config = (flowie_rulesforge_package_config_t){worker, provider, staging_root, 2000u};
    atomic_init(&processing_state.prepare_calls, 0u);
    atomic_init(&processing_state.live, 0u);
    atomic_init(&processing_state.last_closed, 0u);
    atomic_init(&processing_state.reject, 0);
    processing = (flowie_control_release_processing_t){&processing_state, rulesforge_test_prepare,
        rulesforge_test_close};

    flowie_control_test_turbodb_t database;
    release_database = tt_make_temp_file("flowie-rulesforge-release", ".sqlite3");
    check_not_null(release_database);
    check_equal(flowie_control_test_turbodb_init(&database, release_database), 0);
    check_equal(flowie_control_release_store_open(&database.config, &limits, &store), SALTS_OK);
    flowie_control_release_mqtt_config_t deployment = {.store = store, .environment = "qa",
        .domain = "device-a", .realm_name = "domain-release", .resource_uid = "domain-release",
        .owner_name = "flowie-server", .auth_method = "mtls", .identity_provider = &identity,
        .signer = &verify};
    check_equal(flowie_control_release_mqtt_open(&deployment, &mqtt), SALTS_OK);
    check_equal(test_release_https_start(&https, mqtt), SALTS_OK);

    static const char active_route[] = "/domains/device-a/active-release";
    static const chttp_header initial[] = {{"Content-Type", "application/json"},
        {"If-None-Match", "*"}, {"Idempotency-Key", "rulesforge-1"}};
    static const chttp_header replacement[] = {{"Content-Type", "application/json"},
        {"If-Match", "\"release-1\""}, {"Idempotency-Key", "rulesforge-2"}};
    const void *bytes = NULL;
    size_t size = 0;
    uint8_t base[FLOWIE_RELEASE_DIGEST_BYTES], replacement_digest[FLOWIE_RELEASE_DIGEST_BYTES];
    char validate[128], body[96];
    check_equal(rulesforge_test_pack(TEST_RULESFORGE_PROGRAM, NULL, rulesforge_test_acl, NULL,
        &chain.certificate, private_key, &bytes, &size), SALTS_OK);
    rulesforge_test_upload(&https, bytes, size, base, validate, sizeof(validate), body, sizeof(body));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, validate, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 422u);
    check_not_null(strstr(https.response.body, "rulesforge_runtime_required"));
    test_release_https_close(&https);
    flowie_control_release_mqtt_close(mqtt);
    mqtt = NULL;

    deployment.processing = &processing;
    check_equal(flowie_control_release_mqtt_open(&deployment, &mqtt), SALTS_OK);
    check_equal(test_release_https_start(&https, mqtt), SALTS_OK);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, initial, 3u,
        body, strlen(body), 0), SALTS_OK);
    check_equal(https.response.status_code, 409u);
    check_equal(atomic_load(&processing_state.prepare_calls), 0u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, validate, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(atomic_load(&processing_state.live), 0u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, initial, 3u,
        body, strlen(body), 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "Flowie-Runtime-Ready"), "true");
    rulesforge_test_check_acl(mqtt, 1u, SALTS_OK);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, initial, 3u,
        body, strlen(body), 0), SALTS_OK);
    check_equal(chttp_response_header(&https.response, "Idempotency-Replayed"), "true");
    check_equal(atomic_load(&processing_state.prepare_calls), 2u);
    check_equal(atomic_load(&processing_state.live), 1u);

    static const char denied[] = "user writer allow {\n  deny write topic device-a/event\n}";
    check_equal(rulesforge_test_pack(TEST_RULESFORGE_PROGRAM, NULL, denied, base,
        &chain.certificate, private_key, &bytes, &size), SALTS_OK);
    rulesforge_test_upload(&https, bytes, size, replacement_digest, validate, sizeof(validate), body, sizeof(body));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, validate, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    atomic_store(&processing_state.reject, 1);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, replacement, 3u,
        body, strlen(body), 0), SALTS_OK);
    check_equal(https.response.status_code, 503u);
    flowie_control_release_active_t current;
    int ready = 0;
    check_equal(flowie_control_release_mqtt_status(mqtt, &current, &ready), SALTS_OK);
    check_equal(current.generation, 1u);
    check_equal(ready, 1);
    check_equal(atomic_load(&processing_state.live), 1u);
    check_equal(atomic_load(&processing_state.last_closed), 4u);
    rulesforge_test_check_acl(mqtt, 1u, SALTS_OK);

    atomic_store(&processing_state.reject, 0);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, replacement, 3u,
        body, strlen(body), 0), SALTS_OK);
    check_equal(chttp_response_header(&https.response, "ETag"), "\"release-2\"");
    check_equal(atomic_load(&processing_state.live), 1u);
    check_equal(atomic_load(&processing_state.last_closed), 2u);
    rulesforge_test_check_acl(mqtt, 2u, SALTS_EPERM);

    test_release_https_close(&https);
    flowie_control_release_mqtt_close(mqtt);
    mqtt = NULL;
    check_equal(atomic_load(&processing_state.live), 0u);
    check_equal(atomic_load(&processing_state.last_closed), 5u);
    flowie_control_release_store_close(store);
    store = NULL;
    check_equal(flowie_control_release_store_open(&database.config, &limits, &store), SALTS_OK);
    deployment.store = store;
    check_equal(flowie_control_release_mqtt_open(&deployment, &mqtt), SALTS_OK);
    check_equal(flowie_control_release_mqtt_status(mqtt, &current, &ready), SALTS_OK);
    check_equal(ready, 1);
    check_equal(current.generation, 2u);
    check_equal(memcmp(current.digest, replacement_digest, sizeof(current.digest)), 0);
    check_equal(atomic_load(&processing_state.live), 1u);
    rulesforge_test_check_acl(mqtt, 2u, SALTS_EPERM);
  }

}

typedef struct server_test_praktor_s {
  int configured;
  atomic_uint accepted;
  atomic_uint outputs;
  atomic_uint rejected_outputs;
  atomic_int reject_output;
  atomic_int hold_output;
  int rulesforge;
  const char *pin;
  char digest[65];
  char yaml_digest[65];
  uint8_t tar[TEST_TAR_CAPACITY];
  size_t tar_size;
  chttp_application app;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
} server_test_praktor_t;

cmeta_reflect_data(server_test_praktor_t, "flowie.test.PraktorReceiver",
                  cmeta_field(int, configured));
cmeta_component_empty(FlowieTestPraktorReceiver);

static cmeta_status server_test_praktor_create(void *user, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *dependencies, size_t count,
    cmeta_object_ref *out) {
  (void)data;
  (void)config;
  (void)dependencies;
  (void)count;
  return cmeta_object_borrow(out, user, cmeta_reflected_data(server_test_praktor_t), NULL);
}

CHTTP_ACTION(server_test_praktor_receive);
int server_test_praktor_receive(void *self, const chttp_invocation *invocation) {
  server_test_praktor_t *receiver = self;
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  const char *domain = chttp_server_request_header(request, "Flowie-Domain");
  const char *digest = chttp_server_request_header(request, "Flowie-Release-Digest");
  const char *key = chttp_server_request_header(request, "Idempotency-Key");
  const char *type = chttp_server_request_header(request, "Content-Type");
  if (receiver->rulesforge) {
    const char *generation = chttp_server_request_header(request, "Flowie-Generation");
    const char *sink = chttp_server_request_header(request, "Flowie-Sink");
    const unsigned ordinal = atomic_load(&receiver->outputs);
    const char *expected = ordinal == 1u ? "high" : "normal";
    unsigned status = 422u;
    if (request->peer_certificate_sha256 && !strcmp(request->peer_certificate_sha256, receiver->pin) &&
        domain && !strcmp(domain, "device-a") && digest && !strcmp(digest, receiver->digest) &&
        generation && !strcmp(generation, "1") && sink && !strcmp(sink, "alerts") && key &&
        type && !strcmp(type, "application/octet-stream") && request->body_size == strlen(expected) &&
        !memcmp(request->body, expected, request->body_size)) {
      const uint64_t deadline = monotonic_ms() + 1200u;
      while (atomic_load(&receiver->hold_output) && monotonic_ms() < deadline) sleep_ms(1u);
      status = atomic_load(&receiver->hold_output) || atomic_load(&receiver->reject_output) ? 503u : 202u;
      if (status == 202u) atomic_fetch_add(&receiver->outputs, 1u);
      else atomic_fetch_add(&receiver->rejected_outputs, 1u);
    }
    return chttp_invocation_respond(invocation, status, NULL, NULL, 0u);
  }
  const int tar_matches = type && digest && strcmp(type, "application/x-tar") == 0 &&
      strcmp(digest, receiver->digest) == 0 && request->body_size == receiver->tar_size &&
      memcmp(request->body, receiver->tar, receiver->tar_size) == 0;
  const int yaml_matches = type && digest && strcmp(type, "application/yaml") == 0 &&
      strcmp(digest, receiver->yaml_digest) == 0 &&
      request->body_size == sizeof(server_test_praktor_yaml) - 1u &&
      memcmp(request->body, server_test_praktor_yaml, request->body_size) == 0;
  unsigned status = 422u;
  if (request->peer_certificate_sha256 && strcmp(request->peer_certificate_sha256, receiver->pin) == 0 &&
      domain && strcmp(domain, "device-a") == 0 && key && (tar_matches || yaml_matches)) {
    if (strcmp(key, "busy") == 0) status = 503u;
    else if (strcmp(key, "execute-fixture") == 0) {
      atomic_fetch_add(&receiver->accepted, 1u);
      status = 202u;
    }
  }
  return chttp_invocation_respond(invocation, status, NULL, NULL, 0u);
}

static int server_test_praktor_start(server_test_praktor_t *receiver, const test_release_https_t *https) {
  const cnet_tls_server_config tls = {.size = sizeof(tls),
      .cert_file = https->files[TEST_RELEASE_TLS_SERVER_CERT],
      .key_file = https->files[TEST_RELEASE_TLS_SERVER_KEY],
      .ca_file = https->files[TEST_RELEASE_TLS_CA], .client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED};
  receiver->pin = https->mqtt_pin;
  atomic_init(&receiver->accepted, 0u);
  atomic_init(&receiver->outputs, 0u);
  atomic_init(&receiver->rejected_outputs, 0u);
  atomic_init(&receiver->reject_output, 0);
  atomic_init(&receiver->hold_output, 0);
  receiver->provider = (salts_configurator_provider_binding){sizeof(receiver->provider),
      SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(FlowieTestPraktorReceiver),
      receiver, NULL, server_test_praktor_create, NULL, NULL};
  receiver->deployment = (salts_configurator_deployment){&receiver->provider, NULL, NULL};
  const chttp_action action = CHTTP_POST("/jobs", server_test_praktor_receive);
  const chttp_controller controller = {.component_id = "FlowieTestPraktorReceiver",
      .receiver = cmeta_reflected_data(server_test_praktor_t), .actions = &action, .action_count = 1u};
  chttp_application_config app = chttp_application_config_default();
  app.http.tls = &tls;
  app.http.max_request_body_bytes = TEST_TAR_CAPACITY;
  app.http.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  app.http.network.tls_handshake_timeout_ms = 2000;
  app.components = &receiver->deployment;
  app.component_count = 1u;
  app.dependency_capacity = 1u;
  app.controllers = &controller;
  app.controller_count = 1u;
  int rc = chttp_application_init(&receiver->app, &app);
  return rc == SALTS_OK ? chttp_application_start(&receiver->app) : rc;
}

static int server_test_release_sign(const char *anchor_file, const char *leaf_file,
    uint8_t *compressed, size_t *compressed_size, uint8_t *yaml_only, size_t *yaml_only_size) {
  X509_KEY *root_key = test_release_generate_key();
  X509_KEY *leaf_key = test_release_generate_key();
  tls_test_certificate *root = root_key ? test_release_certificate(root_key, NULL, NULL, 0) : NULL;
  tls_test_certificate *leaf = root && leaf_key
      ? test_release_certificate(leaf_key, root, root_key, OID_kp_code_signing) : NULL;
  int rc = SALTS_EIO;
  if (leaf && tt_write_file(anchor_file, root->der, root->size) == 0 &&
      tt_write_file(leaf_file, leaf->der, leaf->size) == 0) {
    uint8_t private_key[SALTS_CRYPTO_SM2_PRIVATE_KEY_SIZE];
    static uint8_t signature[SALTS_CRYPTO_SM2_CMS_MAX_DER];
    char tar[TEST_TAR_CAPACITY];
    sm2_z256_to_bytes(leaf_key->u.sm2_key.private_key, private_key);
    const salts_crypto_sm2_cert_span signer = {leaf->der, leaf->size};
    static const char acl[] = "user writer allow {\n  readwrite topic device-a/event\n}";
    static const test_release_asset_t module = {"wasm/normalize.wasm", server_test_wasm,
                                                 sizeof(server_test_wasm)};
    rc = test_release_mqtt_package(acl, NULL, server_test_praktor_yaml, NULL, private_key, &signer,
        &module, signature, tar, compressed, compressed_size);
    if (rc == SALTS_OK)
      rc = test_release_mqtt_package(acl, NULL, server_test_praktor_yaml, NULL, private_key, &signer,
          NULL, signature, tar, yaml_only, yaml_only_size);
    gmssl_secure_clear(private_key, sizeof(private_key));
  }
  free(leaf);
  free(root);
  tls_test_pki_free_key(leaf_key);
  tls_test_pki_free_key(root_key);
  return rc;
}

static int server_test_signed_pipeline(const char *anchor_file, const char *leaf_file,
    const void **bytes, size_t *size) {
  X509_KEY *root_key = test_release_generate_key();
  X509_KEY *leaf_key = test_release_generate_key();
  tls_test_certificate *root = root_key ? test_release_certificate(root_key, NULL, NULL, 0) : NULL;
  tls_test_certificate *leaf = root && leaf_key
      ? test_release_certificate(leaf_key, root, root_key, OID_kp_code_signing) : NULL;
  int rc = SALTS_EIO;
  if (leaf && tt_write_file(anchor_file, root->der, root->size) == 0 &&
      tt_write_file(leaf_file, leaf->der, leaf->size) == 0) {
    static const char acl[] = "user writer allow {\n  readwrite topic device-a/event\n"
        "  write topic device-a/watermark\n  deny write topic device-a/denied\n}";
    static const char flow[] = "{\"watermark_topic\":\"device-a/watermark\",\"sinks\":[\"alerts\"],"
        "\"sources\":[{\"topic\":\"device-a/event\",\"entry_point\":\"orders\",\"fact_type\":\"Order\"},"
        "{\"topic\":\"device-a/denied\",\"entry_point\":\"orders\",\"fact_type\":\"Order\"}]}";
    const test_release_asset_t asset = {FLOWIE_RULESFORGE_FLOW_PATH, flow, sizeof(flow) - 1u};
    const salts_crypto_sm2_cert_span signer = {leaf->der, leaf->size};
    uint8_t private_key[SALTS_CRYPTO_SM2_PRIVATE_KEY_SIZE];
    sm2_z256_to_bytes(leaf_key->u.sm2_key.private_key, private_key);
    rc = rulesforge_test_pack(TEST_RULESFORGE_PROGRAM, &asset, acl, NULL, &signer, private_key, bytes, size);
    gmssl_secure_clear(private_key, sizeof(private_key));
  }
  free(leaf);
  free(root);
  tls_test_pki_free_key(leaf_key);
  tls_test_pki_free_key(root_key);
  return rc;
}

static int server_test_release_yaml(const char *path, const test_release_https_t *https,
    uint16_t mqtt_port, uint16_t https_port, uint16_t praktor_port, const char *anchor, const char *leaf,
    const char *rulesforge, uint16_t auth_port, uint16_t authority_port) {
  char auth_channels[2048], local_clients[256], praktor[4096] = {0}, management_auth[1536];
  int size;
  if (authority_port) {
    size = snprintf(management_auth, sizeof(management_auth), "        authorization:\n"
        "          url: 'https://127.0.0.1:%u" FLOWIE_RELEASE_ACCESS_PATH "'\n"
        "          ca_file: '%s'\n          timeout_ms: 2000\n",
        (unsigned)authority_port, https->files[TEST_RELEASE_TLS_CA]);
  } else {
    size = snprintf(management_auth, sizeof(management_auth),
        "        operator_id: operator-a\n        operator_sha256: '%s'\n", https->operator_pin);
  }
  if (size < 0 || (size_t)size >= sizeof(management_auth)) return SALTS_ERANGE;
  if (auth_port) {
    size = snprintf(auth_channels, sizeof(auth_channels),
        "channels:\n  mqtt-users:\n    kind: auth_provider\n    config:\n"
        "      backend: https\n      url: 'https://127.0.0.1:%u/v4/authenticate'\n"
        "      method: password\n      service_id: broker-main\n      service_domain: platform-services\n"
        "      service_token_ref: env://FLOWIE_TEST_RELEASE_AUTH_TOKEN\n"
        "      timeout_ms: 2000\n      max_secret_size: 4096\n      tls:\n        ca_file: '%s'\n",
        (unsigned)auth_port, https->files[TEST_RELEASE_TLS_CA]);
    if (size < 0 || (size_t)size >= sizeof(auth_channels)) return SALTS_ERANGE;
    local_clients[0] = '\0';
  } else {
    strcpy(auth_channels, "channels: {}\n");
    size = snprintf(local_clients, sizeof(local_clients),
        "      mqtt_clients:\n        - principal: writer\n          sha256: '%s'\n", https->mqtt_pin);
    if (size < 0 || (size_t)size >= sizeof(local_clients)) return SALTS_ERANGE;
  }
  if (praktor_port) {
    size = snprintf(praktor, sizeof(praktor),
        "      praktor:\n        url: 'https://127.0.0.1:%u/jobs'\n"
        "        ca_file: '%s'\n        cert_file: '%s'\n        key_file: '%s'\n"
        "        timeout_ms: 2000\n",
        (unsigned)praktor_port, https->files[TEST_RELEASE_TLS_CA],
        https->files[TEST_RELEASE_TLS_OTHER_CERT], https->files[TEST_RELEASE_TLS_OTHER_KEY]);
    if (size < 0 || (size_t)size >= sizeof(praktor)) return SALTS_ERANGE;
  }
  char yaml[8192];
  const int length = snprintf(yaml, sizeof(yaml),
      "profiles:\n"
      "  flowie:\n"
      "    endpoint: mqtt\n%s"
      "    domain_release:\n"
      "      domain: device-a\n"
      "      environment: qa\n"
      "      validation_policy_revision: deployment-test\n"
      "      mqtt_client_ca_file: '%s'\n"
      "%s"
      "      management:\n"
      "        host: 127.0.0.1\n"
      "        port: %u\n"
      "        cert_file: '%s'\n"
      "        key_file: '%s'\n"
      "        client_ca_file: '%s'\n"
      "%s"
      "      signer:\n"
      "        reference: signer-a\n"
      "        anchor_der: '%s'\n"
      "        leaf_der: '%s'\n"
      "        cms_identity: '%s'\n"
      "        issuer_identity: '%s'\n"
      "        revocation: not_checked\n"
      "      limits:\n"
      "        package_count: 4\n"
      "        package_bytes: 8388608\n"
      "%s%s"
      "adapters:\n"
      "  mqtt:\n"
      "    config:\n"
      "      transport: tls\n"
      "      host: 127.0.0.1\n"
      "      port: %u\n"
      "      max_connections: 4\n"
      "      max_sessions: 4\n"
      "%s",
      auth_port ? "    auth_provider: mqtt-users\n" : "",
      https->files[TEST_RELEASE_TLS_CA], local_clients, (unsigned)https_port,
      https->files[TEST_RELEASE_TLS_SERVER_CERT], https->files[TEST_RELEASE_TLS_SERVER_KEY],
      https->files[TEST_RELEASE_TLS_CA], management_auth, anchor, leaf, signer_id,
      issuer_id, praktor, rulesforge ? rulesforge : "", (unsigned)mqtt_port, auth_channels);
  return length > 0 && (size_t)length < sizeof(yaml) && tt_write_file(path, yaml, (size_t)length) == 0
      ? SALTS_OK : SALTS_EIO;
}

static int server_test_rulesforge_ack(flowie_test_cnet_client_t *client, uint8_t type,
    uint16_t id, uint8_t *reason) {
  uint8_t header[2], body[16];
  int rc = flowie_test_cnet_recv_exact(client, header, sizeof(header));
  if (rc != SALTS_OK) return rc;
  if (header[0] != type || header[1] < 2u || header[1] > sizeof(body)) return SALTS_EPROTO;
  rc = flowie_test_cnet_recv_exact(client, body, header[1]);
  if (rc != SALTS_OK) return rc;
  if (body[0] != (uint8_t)(id >> 8) || body[1] != (uint8_t)id) return SALTS_EPROTO;
  *reason = header[1] >= 3u ? body[2] : 0;
  return SALTS_OK;
}

static int server_test_rulesforge_publish_packet(flowie_test_cnet_client_t *client, uint16_t id,
    uint8_t qos, uint8_t duplicate, const char *topic, const char *payload, uint8_t *reason) {
  flowie_mqtt_publish_packet_t publish = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
  publish.version = FLOWIE_MQTT_VERSION_5;
  publish.qos = qos;
  publish.duplicate = duplicate;
  publish.packet_id = id;
  publish.topic = (flowie_mqtt_span_t){(const uint8_t *)topic, strlen(topic)};
  publish.payload = (flowie_mqtt_span_t){(const uint8_t *)payload, strlen(payload)};
  uint8_t wire[512];
  size_t size = 0;
  if (flowie_mqtt_publish_packet_encode(&publish, wire, sizeof(wire), &size) != FLOWIE_MQTT_PARSE_OK)
    return SALTS_EPROTO;
  int rc = flowie_test_cnet_send(client, wire, size);
  if (rc != SALTS_OK) return rc;
  rc = server_test_rulesforge_ack(client, qos == 1u ? 0x40u : 0x50u, id, reason);
  if (rc != SALTS_OK || qos != 2u || *reason >= 0x80u) return rc;
  const uint8_t pubrel[] = {0x62u, 0x02u, (uint8_t)(id >> 8), (uint8_t)id};
  rc = flowie_test_cnet_send(client, pubrel, sizeof(pubrel));
  return rc == SALTS_OK ? server_test_rulesforge_ack(client, 0x70u, id, reason) : rc;
}

static int server_test_rulesforge_publish(flowie_test_cnet_client_t *client, uint16_t id,
    uint8_t qos, const char *topic, const char *payload, uint8_t *reason) {
  return server_test_rulesforge_publish_packet(client, id, qos, 0u, topic, payload, reason);
}

static int server_test_wait_counter(const atomic_uint *counter, unsigned int count) {
  const uint64_t deadline = monotonic_ms() + 4000u;
  while (atomic_load(counter) < count && monotonic_ms() < deadline) sleep_ms(1u);
  return atomic_load(counter) == count ? SALTS_OK : SALTS_EIO;
}

static int server_test_rulesforge_journal(const char *path, int completed) {
  flowie_control_database_t *db = NULL;
  if (flowie_control_test_database_open(path, &db) != FLOWIE_CONTROL_DB_OK) return SALTS_EIO;
  static const char query[] = "SELECT "
      "(SELECT count(*) FROM flowie_rule_event WHERE started_at IS NOT NULL AND finished_at IS NOT NULL AND status=0),"
      "(SELECT count(*) FROM flowie_rule_event WHERE started_at IS NOT NULL AND finished_at IS NOT NULL AND status=?),"
      "(SELECT count(*) FROM flowie_rule_output WHERE finished_at IS NOT NULL AND status=0 AND http_status=202 AND sink='alerts'),"
      "(SELECT count(*) FROM flowie_rule_output WHERE finished_at IS NOT NULL AND status=? AND http_status=503 AND sink='alerts'),"
      "(SELECT count(*) FROM flowie_rule_event),"
      "(SELECT count(*) FROM flowie_rule_event WHERE principal<>'writer')";
  const uint64_t deadline = monotonic_ms() + 4000u;
  int result = SALTS_EIO;
  do {
    flowie_control_statement_t *stmt = NULL;
    int status = flowie_control_database_prepare(db, query, -1, &stmt, NULL);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int(stmt, 1, SALTS_EPROTO);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int(stmt, 2, SALTS_EPROTO);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
    if (status == FLOWIE_CONTROL_DB_ROW && flowie_control_database_column_int(stmt, 0) == completed &&
        flowie_control_database_column_int(stmt, 1) == 1 &&
        flowie_control_database_column_int(stmt, 2) == completed &&
        flowie_control_database_column_int(stmt, 3) == 1 &&
        flowie_control_database_column_int(stmt, 4) == completed + 1 &&
        flowie_control_database_column_int(stmt, 5) == 0) result = SALTS_OK;
    flowie_control_database_finalize(stmt);
    if (status != FLOWIE_CONTROL_DB_ROW || result == SALTS_OK) break;
    sleep_ms(1u);
  } while (monotonic_ms() < deadline);
  const int closed = flowie_control_database_close(db);
  return closed == FLOWIE_CONTROL_DB_OK ? result : SALTS_EIO;
}

static int server_test_sql(const char *path, const char *sql) {
  flowie_control_database_t *db = NULL;
  int status = flowie_control_test_database_open(path, &db);
  if (status != FLOWIE_CONTROL_DB_OK) return status;
  flowie_control_statement_t *stmt = NULL;
  status = flowie_control_database_prepare(db, sql, -1, &stmt, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
  flowie_control_database_finalize(stmt);
  const int closed = flowie_control_database_close(db);
  return status == FLOWIE_CONTROL_DB_DONE ? closed : status;
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
  static test_release_https_t release_https;
  static flowie_test_cnet_client_t *release_mqtt;
  static char *release_yaml, *release_anchor, *release_leaf, *release_db;
  static char *rulesforge_journal;
  static char *rulesforge_root;
  static cnet_listener held_port;
  static server_test_praktor_t praktor;
  static server_test_identity_t auth;

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
    flowie_test_cnet_close(release_mqtt);
    release_mqtt = NULL;
    check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);
    if (auth.app.impl) check_equal(chttp_application_close(&auth.app, 0u), SALTS_OK);
    if (auth.peer_pin) {
      (void)server_test_identity_token(NULL);
      auth.peer_pin = NULL;
    }
    if (rulesforge_root) {
      check_equal(fs_rmdir(rulesforge_root), SALTS_OK);
      free(rulesforge_root);
      rulesforge_root = NULL;
    }
    check_equal(chttp_application_close(&praktor.app, 0u), SALTS_OK);
    praktor.rulesforge = 0;
    if (held_port.impl) {
      check_equal(cnet_listener_close(&held_port), SALTS_OK);
      check_equal(cnet_listener_destroy(&held_port), SALTS_OK);
    }
    if (cert_path[0] || key_path[0] || release_yaml) tls_test_clear_server_env();
    test_release_https_close(&release_https);
    flowie_server_config_destroy(config);
    flowie_server_turbodb_config_destroy(database);
    char **files[] = {&release_yaml, &release_anchor, &release_leaf, &release_db, &rulesforge_journal};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
      if (*files[i]) remove(*files[i]);
      free(*files[i]);
      *files[i] = NULL;
    }
    tls_test_remove_file(key_path);
    tls_test_remove_file(cert_path);
  }

  it("runs and recovers the release profile and relays signed WASM bundles over mTLS") {
    check_equal(test_release_https_pki(&release_https), SALTS_OK);
    check_equal(tls_test_set_server_env(release_https.files[TEST_RELEASE_TLS_SERVER_CERT],
        release_https.files[TEST_RELEASE_TLS_SERVER_KEY]), 0);
    release_yaml = tt_make_temp_file("flowie-release-config", ".yml");
    release_anchor = tt_make_temp_file("flowie-release-anchor", ".der");
    release_leaf = tt_make_temp_file("flowie-release-signer", ".der");
    release_db = tt_make_temp_file("flowie-release-db", ".sqlite3");
    check_not_null(release_yaml);
    check_not_null(release_anchor);
    check_not_null(release_leaf);
    check_not_null(release_db);

    static uint8_t compressed[TEST_TAR_CAPACITY * 2u];
    static uint8_t yaml_only[TEST_TAR_CAPACITY * 2u];
    size_t compressed_size, yaml_only_size;
    check_equal(server_test_release_sign(release_anchor, release_leaf, compressed, &compressed_size,
        yaml_only, &yaml_only_size), SALTS_OK);
    uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
    check_equal(salts_crypto_sha256(compressed, compressed_size, digest), SALTS_CRYPTO_OK);
    test_release_hex(digest, praktor.digest);
    check_equal(salts_crypto_sha256(yaml_only, yaml_only_size, digest), SALTS_CRYPTO_OK);
    test_release_hex(digest, praktor.yaml_digest);
    check_equal(zstd_decompress(praktor.tar, sizeof(praktor.tar), &praktor.tar_size,
        compressed, compressed_size), SALTS_OK);
    check_equal(server_test_praktor_start(&praktor, &release_https), SALTS_OK);
    uint16_t praktor_port;
    check_equal(chttp_application_port(&praktor.app, &praktor_port), SALTS_OK);
    static flowie_control_test_turbodb_t sqlite;
    check_equal(flowie_control_test_turbodb_init(&sqlite, release_db), 0);

    const cnet_listener_config listener = {.backend = flowie_test_cnet_backend(),
        .host = "127.0.0.1", .port = 0, .backlog = 2u};
    uint16_t https_port = 0;
    check_equal(cnet_listener_init(&held_port, &listener), SALTS_OK);
    check_equal(cnet_listener_port(&held_port, &https_port), SALTS_OK);
    const uint16_t mqtt_port = flowie_test_cnet_port();
    check_true(mqtt_port != 0u);
    check_equal(server_test_release_yaml(release_yaml, &release_https, mqtt_port, https_port, praktor_port,
        release_anchor, release_leaf, NULL, 0, 0), SALTS_OK);
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    check_equal(flowie_server_config_load(release_yaml, "flowie", 1, &config, &error), SALTS_OK);
    endpoint = *flowie_server_config_endpoint(config);

    check_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        1, &runtime, &operation), SALTS_OK);
    check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);
    check_not_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        0, &runtime, &operation), SALTS_OK);
    check_equal(operation, "release-https-start");
    check_null(runtime);
    check_equal(cnet_listener_close(&held_port), SALTS_OK);
    check_equal(cnet_listener_destroy(&held_port), SALTS_OK);

    check_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        0, &runtime, &operation), SALTS_OK);
    check_equal(flowie_server_runtime_management_port(runtime, &release_https.port), SALTS_OK);
    check_equal(release_https.port, https_port);
    check_equal(test_release_https_client_init(&release_https), SALTS_OK);
    static const char releases[] = "/domains/device-a/releases";
    static const char active[] = "/domains/device-a/active-release";
    static const chttp_header upload[] = {
        {"Content-Type", "application/zstd"}, {"Flowie-Release-Signer", "signer-a"}};
    static const chttp_header activate[] = {{"Content-Type", "application/json"},
        {"If-None-Match", "*"}, {"Idempotency-Key", "activate-deployment"}};
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_GET, active,
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 404u);
    cnet_tls_client_config mqtt_tls = {.size = sizeof(mqtt_tls), .server_name = "localhost",
        .ca_file = release_https.files[TEST_RELEASE_TLS_CA],
        .cert_file = release_https.files[TEST_RELEASE_TLS_OTHER_CERT],
        .key_file = release_https.files[TEST_RELEASE_TLS_OTHER_KEY]};
    uint8_t reason = 0xff;
    release_mqtt = flowie_test_cnet_connect_configured(mqtt_port, 0u, &mqtt_tls);
    check_not_null(release_mqtt);
    check_equal(test_release_mqtt_connect_client(release_mqtt, "writer", NULL, &reason), SALTS_OK);
    check_equal(reason, 0x88u);
    flowie_test_cnet_close(release_mqtt);
    release_mqtt = NULL;
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, releases, upload, 2u,
        compressed, compressed_size, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 201u);
    char validate[128], submit[128], body[96];
    snprintf(validate, sizeof(validate), "%s/%s/validate", releases, praktor.digest);
    snprintf(submit, sizeof(submit), "%s/%s/praktor", releases, praktor.digest);
    snprintf(body, sizeof(body), "{\"digest\":\"%s\"}", praktor.digest);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, validate,
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 200u);
    check_equal(atomic_load(&praktor.accepted), 0u);
    static const chttp_header execute[] = {{"Idempotency-Key", "execute-fixture"}};
    static const chttp_header busy[] = {{"Idempotency-Key", "busy"}};
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, submit,
        execute, 1u, NULL, 0u, 1), SALTS_OK);
    check_equal(release_https.response.status_code, 403u);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, submit,
        busy, 1u, NULL, 0u, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 503u);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, submit,
        execute, 1u, NULL, 0u, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 202u);
    check_equal(atomic_load(&praktor.accepted), 1u);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_PUT, active, activate, 3u,
        body, strlen(body), 0), SALTS_OK);
    check_equal(release_https.response.status_code, 200u);
    check_equal(atomic_load(&praktor.accepted), 1u);

    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, releases, upload, 2u,
        yaml_only, yaml_only_size, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 201u);
    snprintf(submit, sizeof(submit), "%s/%s/praktor", releases, praktor.yaml_digest);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, submit,
        execute, 1u, NULL, 0u, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 202u);
    check_equal(atomic_load(&praktor.accepted), 2u);

    mqtt_tls.cert_file = release_https.files[TEST_RELEASE_TLS_OPERATOR_CERT];
    mqtt_tls.key_file = release_https.files[TEST_RELEASE_TLS_OPERATOR_KEY];
    release_mqtt = flowie_test_cnet_connect_configured(mqtt_port, 0u, &mqtt_tls);
    check_not_null(release_mqtt);
    check_equal(test_release_mqtt_connect_client(release_mqtt, "writer", NULL, &reason), SALTS_OK);
    check_equal(reason, 0x86u);
    flowie_test_cnet_close(release_mqtt);
    release_mqtt = NULL;
    check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);

    check_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        0, &runtime, &operation), SALTS_OK);
    mqtt_tls.cert_file = release_https.files[TEST_RELEASE_TLS_OTHER_CERT];
    mqtt_tls.key_file = release_https.files[TEST_RELEASE_TLS_OTHER_KEY];
    release_mqtt = flowie_test_cnet_connect_configured(mqtt_port, 0u, &mqtt_tls);
    check_not_null(release_mqtt);
    check_equal(test_release_mqtt_connect_client(release_mqtt, "writer", NULL, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(test_release_mqtt_publish(release_mqtt, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(test_release_mqtt_subscribe(release_mqtt, &reason), SALTS_OK);
    check_equal(reason, 0u);
  }

  it("authenticates via HTTPS and authorizes the returned identity with the signed ACL") {
    check_equal(test_release_https_pki(&release_https), SALTS_OK);
    check_equal(tls_test_set_server_env(release_https.files[TEST_RELEASE_TLS_SERVER_CERT],
        release_https.files[TEST_RELEASE_TLS_SERVER_KEY]), 0);
    release_yaml = tt_make_temp_file("flowie-release-auth-config", ".yml");
    release_anchor = tt_make_temp_file("flowie-release-auth-anchor", ".der");
    release_leaf = tt_make_temp_file("flowie-release-auth-signer", ".der");
    release_db = tt_make_temp_file("flowie-release-auth-db", ".sqlite3");
    check_not_null(release_yaml);
    check_not_null(release_anchor);
    check_not_null(release_leaf);
    check_not_null(release_db);
    static uint8_t compressed[TEST_TAR_CAPACITY * 2u], spare[TEST_TAR_CAPACITY * 2u];
    size_t size, spare_size;
    check_equal(server_test_release_sign(release_anchor, release_leaf, compressed, &size,
        spare, &spare_size), SALTS_OK);
    check_equal(server_test_identity_start(&auth, &release_https), SALTS_OK);
    check_true(auth.port != 0u);
    const uint16_t mqtt_port = flowie_test_cnet_port();
    check_true(mqtt_port != 0u);
    check_equal(server_test_release_yaml(release_yaml, &release_https, mqtt_port, 0, 0,
        release_anchor, release_leaf, NULL, auth.port, 0), SALTS_OK);
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    check_equal(flowie_server_config_load(release_yaml, "flowie", 1, &config, &error), SALTS_OK);
    endpoint = *flowie_server_config_endpoint(config);
    flowie_control_test_turbodb_t sqlite;
    check_equal(flowie_control_test_turbodb_init(&sqlite, release_db), 0);
    check_equal(server_test_identity_token(NULL), 0);
    check_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        1, &runtime, &operation), SALTS_ENOENT);
    check_equal(operation, "release-auth-prepare");
    check_null(runtime);
    check_equal(server_test_identity_token("test-release-token"), 0);
    check_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        0, &runtime, &operation), SALTS_OK);
    check_equal(flowie_server_runtime_management_port(runtime, &release_https.port), SALTS_OK);
    check_equal(test_release_https_client_init(&release_https), SALTS_OK);
    uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
    char validate[128], body[96];
    rulesforge_test_upload(&release_https, compressed, size, digest, validate, sizeof(validate), body, sizeof(body));
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, validate,
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 200u);
    static const chttp_header activate[] = {{"Content-Type", "application/json"},
        {"If-None-Match", "*"}, {"Idempotency-Key", "remote-identity"}};
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_PUT, "/domains/device-a/active-release",
        activate, 3u, body, strlen(body), 0), SALTS_OK);
    check_equal(release_https.response.status_code, 200u);

    cnet_tls_client_config tls = {.size = sizeof(tls), .server_name = "localhost",
        .ca_file = release_https.files[TEST_RELEASE_TLS_CA],
        .cert_file = release_https.files[TEST_RELEASE_TLS_OTHER_CERT],
        .key_file = release_https.files[TEST_RELEASE_TLS_OTHER_KEY]};
    const struct { const char *password; uint8_t reason; unsigned int mode; } cases[] = {
        {"motor", 0u, 0u}, {"invalid", 0x86u, 0u}, {"motor", 0x86u, 1u}, {"motor", 0x88u, 2u}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      atomic_store(&auth.mode, cases[i].mode);
      release_mqtt = flowie_test_cnet_connect_configured(mqtt_port, 0u, &tls);
      check_not_null(release_mqtt);
      uint8_t reason = 0xff;
      check_equal(test_release_mqtt_connect_client(release_mqtt, "registration-42", cases[i].password, &reason),
          SALTS_OK);
      check_equal(reason, cases[i].reason);
      if (i == 0u) {
        check_equal(test_release_mqtt_publish(release_mqtt, &reason), SALTS_OK);
        check_equal(reason, 0u);
        check_equal(server_test_rulesforge_publish(release_mqtt, 2u, 1u, "device-a/denied", "x", &reason),
            SALTS_OK);
        check_equal(reason, 0x87u);
      }
      flowie_test_cnet_close(release_mqtt);
      release_mqtt = NULL;
    }
    check_equal(atomic_load(&auth.calls), 4u);
    tls.cert_file = release_https.files[TEST_RELEASE_TLS_OPERATOR_CERT];
    tls.key_file = release_https.files[TEST_RELEASE_TLS_OPERATOR_KEY];
    release_mqtt = flowie_test_cnet_connect_configured(mqtt_port, 0u, &tls);
    check_not_null(release_mqtt);
    uint8_t reason = 0xff;
    check_equal(test_release_mqtt_connect_client(release_mqtt, "registration-42", "motor", &reason), SALTS_OK);
    check_equal(reason, 0x86u);
    check_equal(atomic_load(&auth.calls), 4u);
  }

  it("enforces remote package permissions and records the activating account") {
    check_equal(test_release_https_pki(&release_https), SALTS_OK);
    check_equal(tls_test_set_server_env(release_https.files[TEST_RELEASE_TLS_SERVER_CERT],
        release_https.files[TEST_RELEASE_TLS_SERVER_KEY]), 0);
    release_yaml = tt_make_temp_file("flowie-release-authority", ".yml");
    release_anchor = tt_make_temp_file("flowie-release-authority-anchor", ".der");
    release_leaf = tt_make_temp_file("flowie-release-authority-signer", ".der");
    release_db = tt_make_temp_file("flowie-release-authority-store", ".sqlite3");
    check_not_null(release_yaml);
    check_not_null(release_anchor);
    check_not_null(release_leaf);
    check_not_null(release_db);
    static uint8_t archive[TEST_TAR_CAPACITY * 2u], spare[TEST_TAR_CAPACITY * 2u];
    size_t size, spare_size;
    check_equal(server_test_release_sign(release_anchor, release_leaf, archive, &size,
        spare, &spare_size), SALTS_OK);
    check_equal(server_test_identity_start(&auth, &release_https), SALTS_OK);
    const uint16_t mqtt_port = flowie_test_cnet_port();
    check_true(mqtt_port != 0u);
    check_equal(server_test_release_yaml(release_yaml, &release_https, mqtt_port, 0, 0,
        release_anchor, release_leaf, NULL, 0, auth.port), SALTS_OK);
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    check_equal(flowie_server_config_load(release_yaml, "flowie", 1, &config, &error), SALTS_OK);
    endpoint = *flowie_server_config_endpoint(config);
    flowie_control_test_turbodb_t sqlite;
    check_equal(flowie_control_test_turbodb_init(&sqlite, release_db), 0);
    check_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        0, &runtime, &operation), SALTS_OK);
    check_equal(flowie_server_runtime_management_port(runtime, &release_https.port), SALTS_OK);
    check_equal(test_release_https_client_init(&release_https), SALTS_OK);
    static const char releases[] = "/domains/device-a/releases";
    static const char active[] = "/domains/device-a/active-release";
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_GET, releases, NULL, 0,
        NULL, 0, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 401u);
    static const chttp_header ci[] = {{"Authorization", "Bearer package-ci"},
        {"X-Flowie-Service-Domain", "device-a"}, {"X-Flowie-Service-Id", "release-ci"},
        {"Content-Type", "application/zstd"}, {"Flowie-Release-Signer", "signer-a"}};
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, releases,
        ci, 5u, archive, size, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 201u);
    uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
    char hex[65], validate[128], body[96];
    check_equal(salts_crypto_sha256(archive, size, digest), SALTS_CRYPTO_OK);
    test_release_hex(digest, hex);
    snprintf(validate, sizeof(validate), "%s/%s/validate", releases, hex);
    snprintf(body, sizeof(body), "{\"digest\":\"%s\"}", hex);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, validate,
        ci, 3u, NULL, 0, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 200u);
    const chttp_header activate[] = {ci[0], ci[1], ci[2], {"Content-Type", "application/json"},
        {"If-None-Match", "*"}, {"Idempotency-Key", "ci-denied"}};
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_PUT, active,
        activate, 6u, body, strlen(body), 0), SALTS_OK);
    check_equal(release_https.response.status_code, 403u);
    static const chttp_header owner[] = {{"Authorization", "Bearer package-owner"},
        {"Content-Type", "application/json"}, {"If-None-Match", "*"}, {"Idempotency-Key", "owner-activate"}};
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_PUT, active,
        owner, 4u, body, strlen(body), 0), SALTS_OK);
    check_equal(release_https.response.status_code, 200u);
    check_equal(server_test_sql(release_db, "SELECT actor FROM flowie_domain_release_activation "
        "WHERE actor='8:device-a/release-owner'"), FLOWIE_CONTROL_DB_ROW);
    atomic_store(&auth.mode, 1u);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, validate,
        ci, 3u, NULL, 0, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 403u);
    check_equal(chttp_application_close(&auth.app, 0u), SALTS_OK);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_GET, active,
        owner, 1u, NULL, 0, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 503u);
  }

  it("authenticates MQTT through HTTPS and delivers signed RulesForge output over mTLS") {
    const char *worker = getenv("FLOWIE_TEST_RULESFORGE_EXECUTABLE");
    const char *provider = getenv("FLOWIE_TEST_RULESFORGE_PROVIDER");
    if (!provider) return; /* The separate SDK is selected by the integration test environment. */
    check_not_null(worker);
    check_equal(test_release_https_pki(&release_https), SALTS_OK);
    check_equal(tls_test_set_server_env(release_https.files[TEST_RELEASE_TLS_SERVER_CERT],
        release_https.files[TEST_RELEASE_TLS_SERVER_KEY]), 0);
    release_yaml = tt_make_temp_file("flowie-rulesforge-profile", ".yml");
    release_anchor = tt_make_temp_file("flowie-rulesforge-anchor", ".der");
    release_leaf = tt_make_temp_file("flowie-rulesforge-signer", ".der");
    release_db = tt_make_temp_file("flowie-rulesforge-production", ".sqlite3");
    rulesforge_journal = tt_make_temp_file("flowie-rulesforge-journal", ".sqlite3");
    rulesforge_root = tt_make_temp_dir("flowie-rulesforge-production");
    check_not_null(release_yaml);
    check_not_null(release_anchor);
    check_not_null(release_leaf);
    check_not_null(release_db);
    check_not_null(rulesforge_journal);
    check_not_null(rulesforge_root);
    const void *bytes = NULL;
    size_t size = 0;
    check_equal(server_test_signed_pipeline(release_anchor, release_leaf, &bytes, &size), SALTS_OK);
    uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
    check_equal(salts_crypto_sha256(bytes, size, digest), SALTS_CRYPTO_OK);
    test_release_hex(digest, praktor.digest);
    praktor.rulesforge = 1;
    check_equal(server_test_praktor_start(&praktor, &release_https), SALTS_OK);
    check_equal(server_test_identity_start(&auth, &release_https), SALTS_OK);
    check_equal(server_test_identity_token("test-release-token"), 0);
    uint16_t sink_port = 0;
    check_equal(chttp_application_port(&praktor.app, &sink_port), SALTS_OK);
    const uint16_t mqtt_port = flowie_test_cnet_port();
    check_true(mqtt_port != 0u);
    char processing_yaml[4096];
    const int written = snprintf(processing_yaml, sizeof(processing_yaml),
        "      rulesforge:\n        executable: '%s'\n        provider: '%s'\n        staging_root: '%s'\n"
        "        journal_file: '%s'\n"
        "        timeout_ms: 2000\n        sinks:\n          - name: alerts\n"
        "            url: 'https://127.0.0.1:%u/jobs'\n            ca_file: '%s'\n"
        "            cert_file: '%s'\n            key_file: '%s'\n",
        worker, provider, rulesforge_root, rulesforge_journal, (unsigned)sink_port, release_https.files[TEST_RELEASE_TLS_CA],
        release_https.files[TEST_RELEASE_TLS_OTHER_CERT], release_https.files[TEST_RELEASE_TLS_OTHER_KEY]);
    check_true(written > 0 && (size_t)written < sizeof(processing_yaml));
    check_equal(server_test_release_yaml(release_yaml, &release_https, mqtt_port, 0, 0,
        release_anchor, release_leaf, processing_yaml, auth.port, 0), SALTS_OK);
    flowie_server_config_error_t error = FLOWIE_SERVER_CONFIG_ERROR_INIT;
    check_equal(flowie_server_config_load(release_yaml, "flowie", 1, &config, &error), SALTS_OK);
    endpoint = *flowie_server_config_endpoint(config);
    flowie_control_test_turbodb_t sqlite;
    check_equal(flowie_control_test_turbodb_init(&sqlite, release_db), 0);
    check_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        0, &runtime, &operation), SALTS_OK);
    check_equal(flowie_server_runtime_management_port(runtime, &release_https.port), SALTS_OK);
    check_equal(test_release_https_client_init(&release_https), SALTS_OK);
    char validate[128], body[96];
    rulesforge_test_upload(&release_https, bytes, size, digest, validate, sizeof(validate), body, sizeof(body));
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_POST, validate,
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(release_https.response.status_code, 200u);
    static const chttp_header activate[] = {{"Content-Type", "application/json"},
        {"If-None-Match", "*"}, {"Idempotency-Key", "rules-production"}};
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_PUT, "/domains/device-a/active-release",
        activate, 3u, body, strlen(body), 0), SALTS_OK);
    check_equal(release_https.response.status_code, 200u);

    const cnet_tls_client_config tls = {.size = sizeof(tls), .server_name = "localhost",
        .ca_file = release_https.files[TEST_RELEASE_TLS_CA],
        .cert_file = release_https.files[TEST_RELEASE_TLS_OTHER_CERT],
        .key_file = release_https.files[TEST_RELEASE_TLS_OTHER_KEY]};
    release_mqtt = flowie_test_cnet_connect_configured(mqtt_port, 0u, &tls);
    check_not_null(release_mqtt);
    uint8_t reason = 0xff;
    uint8_t session_present = 0xffu;
    check_equal(test_release_mqtt_connect_session(release_mqtt, "registration-42", "motor", 1u,
        &session_present, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(session_present, 0u);
    static const char later[] = "{\"id\":\"later\",\"time_ms\":200,\"data\":{\"amount\":15,\"total\":0,\"done\":false}}";
    static const char earlier[] = "{\"id\":\"earlier\",\"time_ms\":100,\"data\":{\"amount\":15,\"total\":0,\"done\":false}}";
    check_equal(server_test_rulesforge_publish(release_mqtt, 1, 1, "device-a/denied", later, &reason), SALTS_OK);
    check_equal(reason, 0x87u);
    check_equal(atomic_load(&praktor.outputs), 0u);
    atomic_store(&praktor.hold_output, 1);
    check_equal(server_test_rulesforge_publish(release_mqtt, 2, 2, "device-a/event", later, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(atomic_load(&praktor.outputs), 0u);
    atomic_store(&praktor.hold_output, 0);
    check_equal(server_test_wait_counter(&praktor.outputs, 1u), SALTS_OK);
    check_equal(server_test_rulesforge_publish(release_mqtt, 3, 1, "device-a/event", earlier, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(server_test_wait_counter(&praktor.outputs, 2u), SALTS_OK);
    check_equal(server_test_rulesforge_publish(release_mqtt, 4, 1, "device-a/watermark", "{\"time_ms\":201}",
        &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(server_test_wait_counter(&praktor.outputs, 3u), SALTS_OK);
    atomic_store(&praktor.reject_output, 1);
    static const char rejected[] = "{\"id\":\"rejected\",\"time_ms\":202,\"data\":{\"amount\":1,\"total\":0,\"done\":false}}";
    reason = 0xff;
    check_equal(server_test_rulesforge_publish(release_mqtt, 5, 1, "device-a/event", rejected, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(server_test_wait_counter(&praktor.rejected_outputs, 1u), SALTS_OK);
    check_equal(atomic_load(&praktor.outputs), 3u);
    check_equal(server_test_rulesforge_journal(rulesforge_journal, 3), SALTS_OK);
    check_equal(test_release_https_request(&release_https, CHTTP_METHOD_GET, "/domains/device-a/status",
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_not_null(strstr(release_https.response.body, "\"ready\":true"));

    atomic_store(&praktor.reject_output, 0);
    static const char accepted[] = "{\"id\":\"accepted\",\"time_ms\":203,\"data\":{\"amount\":1,\"total\":0,\"done\":false}}";
    check_equal(server_test_sql(rulesforge_journal, "CREATE TABLE test_finish_fault (seen INTEGER)"),
        FLOWIE_CONTROL_DB_OK);
    check_equal(server_test_sql(rulesforge_journal, "CREATE TRIGGER test_finish_fail BEFORE UPDATE OF finished_at "
        "ON flowie_rule_event WHEN OLD.sequence=5 BEGIN INSERT INTO test_finish_fault VALUES(1); "
        "SELECT RAISE(FAIL,'injected finish failure'); END"), FLOWIE_CONTROL_DB_OK);
    check_equal(server_test_rulesforge_publish(release_mqtt, 6, 1, "device-a/event", accepted, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(server_test_wait_counter(&praktor.outputs, 4u), SALTS_OK);
    const uint64_t fault_deadline = monotonic_ms() + 2000u;
    int fault_status;
    do {
      fault_status = server_test_sql(rulesforge_journal, "SELECT seen FROM test_finish_fault LIMIT 1");
      if (fault_status == FLOWIE_CONTROL_DB_ROW) break;
      sleep_ms(10u);
    } while (monotonic_ms() < fault_deadline);
    check_equal(fault_status, FLOWIE_CONTROL_DB_ROW);
    check_equal(server_test_sql(rulesforge_journal, "DROP TRIGGER test_finish_fail"), FLOWIE_CONTROL_DB_OK);
    check_equal(server_test_rulesforge_journal(rulesforge_journal, 4), SALTS_OK);
    check_equal(atomic_load(&praktor.outputs), 4u);

    /* Fail only the repository write that would commit PUBREC after journal admission. */
    check_equal(server_test_sql(release_db, "CREATE TRIGGER test_pubrec_fail BEFORE INSERT "
        "ON flowie_server_inflight WHEN NEW.packet_id=91 "
        "BEGIN SELECT RAISE(ABORT,'injected PUBREC failure'); END"), FLOWIE_CONTROL_DB_OK);
    static const char replayed[] = "{\"id\":\"replayed\",\"time_ms\":204,\"data\":{\"amount\":1,\"total\":0,\"done\":false}}";
    check_not_equal(server_test_rulesforge_publish(release_mqtt, 91, 2, "device-a/event", replayed, &reason), SALTS_OK);
    check_equal(server_test_wait_counter(&praktor.outputs, 5u), SALTS_OK);
    check_equal(server_test_rulesforge_journal(rulesforge_journal, 5), SALTS_OK);
    check_equal(server_test_sql(release_db, "DROP TRIGGER test_pubrec_fail"), FLOWIE_CONTROL_DB_OK);
    flowie_test_cnet_close(release_mqtt);
    release_mqtt = NULL;
    check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);

    check_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        0, &runtime, &operation), SALTS_OK);
    release_mqtt = flowie_test_cnet_connect_configured(mqtt_port, 0u, &tls);
    check_not_null(release_mqtt);
    check_equal(test_release_mqtt_connect_session(release_mqtt, "registration-42", "motor", 0u,
        &session_present, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(session_present, 1u);
    check_equal(server_test_rulesforge_publish_packet(release_mqtt, 91, 2, 1u,
        "device-a/event", replayed, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(server_test_rulesforge_journal(rulesforge_journal, 5), SALTS_OK);
    static const char reused[] = "{\"id\":\"reused\",\"time_ms\":205,\"data\":{\"amount\":1,\"total\":0,\"done\":false}}";
    check_equal(server_test_rulesforge_publish(release_mqtt, 91, 2, "device-a/event", reused, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(server_test_wait_counter(&praktor.outputs, 6u), SALTS_OK);
    check_equal(server_test_rulesforge_journal(rulesforge_journal, 6), SALTS_OK);

    flowie_test_cnet_close(release_mqtt);
    release_mqtt = NULL;
    check_equal(flowie_server_runtime_destroy(&runtime), SALTS_OK);
    flowie_control_test_turbodb_t journal_database;
    check_equal(flowie_control_test_turbodb_init(&journal_database, rulesforge_journal), 0);
    flowie_control_release_store_t *journal = NULL;
    check_equal(flowie_control_release_journal_open(&journal_database.config, &journal), SALTS_OK);
    const flowie_release_journal_scope_t scope = {"qa", "device-a", 1u, digest};
    static const char queued[] = "{\"id\":\"queued\",\"time_ms\":206,\"data\":{\"amount\":1,\"total\":0,\"done\":false}}";
    const int enqueued = flowie_control_release_enqueue(journal, &scope, "writer", "device-a/event",
        queued, sizeof(queued) - 1u, NULL);
    flowie_control_release_store_close(journal);
    check_equal(enqueued, SALTS_OK);

    flowie_server_config_destroy(config);
    config = NULL;
    check_equal(server_test_release_yaml(release_yaml, &release_https, mqtt_port, sink_port, 0,
        release_anchor, release_leaf, processing_yaml, auth.port, 0), SALTS_OK);
    check_equal(flowie_server_config_load(release_yaml, "flowie", 1, &config, &error), SALTS_OK);
    endpoint = *flowie_server_config_endpoint(config);
    check_not_equal(flowie_server_runtime_create(config, &endpoint, &sqlite.config, &options,
        0, &runtime, &operation), SALTS_OK);
    check_equal(operation, "release-https-start");
    check_null(runtime);
    check_equal(server_test_sql(rulesforge_journal,
        "SELECT sequence FROM flowie_rule_event WHERE sequence=8 AND started_at IS NULL"),
        FLOWIE_CONTROL_DB_ROW);
    check_equal(atomic_load(&praktor.outputs), 6u);
    check_equal(atomic_load(&auth.calls), 2u);
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
