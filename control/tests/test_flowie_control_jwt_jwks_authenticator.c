#include "flowie_control_jwt_jwks_authenticator_internal.h"

#include "cjwt/cjwt.h"
#include "mtls_test_server.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include "salts_error.h"
#include <json_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t JWT_TEST_PRIVATE_KEY[] = {
    0x6c, 0x82, 0xa5, 0x62, 0xcb, 0x80, 0x8d, 0x10, 0xd6, 0x32, 0xbe, 0x89, 0xc8, 0x51, 0x3e,
    0xbf, 0x6c, 0x92, 0x9f, 0x34, 0xdd, 0xfa, 0x8c, 0x9f, 0x63, 0xc9, 0x96, 0x0e, 0xf6, 0xe3,
    0x48, 0xa3, 0x52, 0x8c, 0x8a, 0x3f, 0xcc, 0x2f, 0x04, 0x4e, 0x39, 0xa3, 0xfc, 0x5b, 0x94,
    0x49, 0x2f, 0x8f, 0x03, 0x2e, 0x75, 0x49, 0xa2, 0x00, 0x98, 0xf9, 0x5b};

static const char JWT_TEST_JWKS[] =
    "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed448\",\"kid\":\"key-1\","
    "\"use\":\"sig\",\"alg\":\"EdDSA\","
    "\"x\":\"X9dEm1m0Yf0s54fsYWrUah2hNCSFpw4fig6nXYDpZ3jt8SR2m0bHBhvWeD3x5Q9s0foavq_oJWGA\"}]}";

typedef struct jwt_test_claims_s {
  const char *issuer;
  const char *audience;
  const char *subject;
  const char *domain_id;
  const char *kid;
  cjwt_alg_t algorithm;
  int64_t issued_at;
  int64_t not_before;
  int64_t expires_at;
  uint64_t revision;
  uint32_t assurance_level;
  int account_enabled;
} jwt_test_claims_t;

static flowie_control_jwt_jwks_authenticator_t *jwt_test_authenticator(void) {
  flowie_control_jwt_jwks_authenticator_config_t config =
      FLOWIE_CONTROL_JWT_JWKS_AUTHENTICATOR_CONFIG_INIT;
  flowie_control_jwt_jwks_authenticator_t *authenticator = NULL;
  config.url = "https://idp.example/.well-known/jwks.json";
  config.method = "jwt";
  config.trusted_issuer = "https://idp.example";
  config.audience = "flowie";
  config.subject_type = "device";
  config.algorithm = "EdDSA";
  check_equal(flowie_control_jwt_jwks_authenticator_create(&config, &authenticator), SALTS_OK);
  check_not_null(authenticator);
  return authenticator;
}

static char *jwt_test_encode(const jwt_test_claims_t *claims) {
  cjwt_t token = {0};
  json_value_t *groups = NULL;
  char *encoded = NULL;
  char *audiences[1];
  check_not_null(claims);
  token.header.alg = claims->algorithm;
  token.header.kid = (char *)claims->kid;
  token.iss = (char *)claims->issuer;
  token.sub = (char *)claims->subject;
  audiences[0] = (char *)claims->audience;
  token.aud.count = 1;
  token.aud.names = audiences;
  token.iat = (int64_t *)&claims->issued_at;
  token.nbf = (int64_t *)&claims->not_before;
  token.exp = (int64_t *)&claims->expires_at;
  token.private_claims = json_create_object();
  check_not_null(token.private_claims);
  json_object_set_string(token.private_claims, "domain_id", claims->domain_id);
  json_object_set_bool(token.private_claims, "account_enabled", claims->account_enabled != 0);
  json_object_set_number(token.private_claims, "revision", (double)claims->revision);
  json_object_set_number(token.private_claims, "assurance_level",
                               (double)claims->assurance_level);
  groups = json_create_array();
  check_not_null(groups);
  json_array_add(groups, json_create_string("operators"));
  json_object_add(token.private_claims, "groups", groups);
  check_equal(cjwt_encode(&token, JWT_TEST_PRIVATE_KEY, sizeof(JWT_TEST_PRIVATE_KEY), &encoded),
              CJWTE_OK);
  check_not_null(encoded);
  json_free(token.private_claims);
  return encoded;
}

static jwt_test_claims_t jwt_test_valid_claims(void) {
  jwt_test_claims_t claims = {"https://idp.example",
                              "flowie",
                              "device-a",
                              "root-a",
                              "key-1",
                              alg_eddsa,
                              990u,
                              990u,
                              1100u,
                              7u,
                              2u,
                              1};
  return claims;
}

static flowie_control_external_auth_request_t jwt_test_request(const char *token) {
  flowie_control_external_auth_request_t request = FLOWIE_CONTROL_EXTERNAL_AUTH_REQUEST_INIT;
  request.domain_id = "root-a";
  request.presented_identity = "device-a";
  request.method = "jwt";
  request.secret = (const uint8_t *)token;
  request.secret_size = strlen(token);
  request.protocol = "mqtt";
  request.remote_address = "192.0.2.10:1883";
  return request;
}

static uint64_t jwt_test_clock(void *ctx) { return ctx ? *(const uint64_t *)ctx : 0u; }

static int jwt_test_http_response(char *buffer, size_t capacity, const char *content_type) {
  int result;
  if (!buffer || capacity == 0u || !content_type) return -1;
  result = snprintf(buffer, capacity,
                    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                    "Connection: close\r\n\r\n%s",
                    content_type, sizeof(JWT_TEST_JWKS) - 1u, JWT_TEST_JWKS);
  return result > 0 && (size_t)result < capacity ? result : -1;
}

static int jwt_test_network_verify(const char *content_type, uint8_t *request_out,
                                   size_t request_capacity) {
  char cert_file[512] = {0};
  char key_file[512] = {0};
  char response[1024];
  char url[192];
  flow_mtls_test_server_t server;
  flowie_control_jwt_jwks_authenticator_config_t config =
      FLOWIE_CONTROL_JWT_JWKS_AUTHENTICATOR_CONFIG_INIT;
  flowie_control_jwt_jwks_authenticator_t *authenticator = NULL;
  const flowie_control_external_authenticator_t *interface = NULL;
  flowie_control_external_auth_request_t request;
  flowie_control_external_auth_assertion_t assertion =
      FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  jwt_test_claims_t claims = jwt_test_valid_claims();
  uint64_t now = 1000u;
  char *token = NULL;
  int response_size;
  int rc = SALTS_EIO;
  memset(&server, 0, sizeof(server));
  server.listener = FLOW_MTLS_TEST_INVALID_SOCKET;
  response_size = jwt_test_http_response(response, sizeof(response), content_type);
  if (response_size <= 0 ||
      tls_test_write_server_files(cert_file, sizeof(cert_file), key_file, sizeof(key_file)) != 0)
    goto done;
  token = jwt_test_encode(&claims);
  if (!token) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  if (flow_tls_test_server_start(&server, (const uint8_t *)response, (size_t)response_size) != 0)
    goto done;
  if (snprintf(url, sizeof(url), "https://localhost:%u/.well-known/jwks.json", server.port) <= 0)
    goto done;
  config.url = url;
  config.method = "jwt";
  config.trusted_issuer = "https://idp.example";
  config.audience = "flowie";
  config.subject_type = "device";
  config.algorithm = "EdDSA";
  config.ca_file = cert_file;
  config.clock_seconds = jwt_test_clock;
  config.clock_ctx = &now;
  rc = flowie_control_jwt_jwks_authenticator_create(&config, &authenticator);
  if (rc != SALTS_OK) goto done;
  interface = flowie_control_jwt_jwks_authenticator_interface(authenticator);
  request = jwt_test_request(token);
  rc = interface->verify(interface->ctx, &request, &assertion);

done:
  flowie_control_jwt_jwks_authenticator_destroy(authenticator);
  flow_mtls_test_server_join(&server);
  if (request_out && request_capacity != 0u && server.request_size < request_capacity)
    memcpy(request_out, server.request, server.request_size + 1u);
  free(token);
  tls_test_remove_file(key_file);
  tls_test_remove_file(cert_file);
  return rc;
}

spec("Flowie control JWT/JWKS authenticator") {
  it("fetches its first JWKS over CHTTP and rejects a wrong media type") {
    uint8_t request[FLOW_MTLS_TEST_REQUEST_CAPACITY] = {0};
    check_equal(jwt_test_network_verify("Application/JSON", request, sizeof(request)), SALTS_OK);
    check_not_null(strstr((const char *)request, "GET /.well-known/jwks.json HTTP/1.1"));
    check_equal(jwt_test_network_verify("text/plain", NULL, 0u), SALTS_EPROTO);
  }

  it("verifies one exact asymmetric key and emits a Domain-bound assertion") {
    flowie_control_jwt_jwks_authenticator_t *authenticator = jwt_test_authenticator();
    jwt_test_claims_t claims = jwt_test_valid_claims();
    char *token = jwt_test_encode(&claims);
    flowie_control_external_auth_request_t request = jwt_test_request(token);
    flowie_control_external_auth_assertion_t assertion =
        FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;

    check_equal(flowie_control_jwt_jwks_authenticator_install(authenticator, JWT_TEST_JWKS,
                                                              sizeof(JWT_TEST_JWKS) - 1u, 1200u),
                SALTS_OK);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_OK);
    check_equal(assertion.issuer, "https://idp.example");
    check_equal(assertion.domain_id, "root-a");
    check_equal(assertion.subject, "device-a");
    check_equal(assertion.auth_method, "jwt");
    check_equal(assertion.revision, 7u);
    check_equal(assertion.external_group_count, 1u);
    check_equal(assertion.external_groups[0], "operators");
    request.domain_id = "";
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_OK);
    check_equal(assertion.domain_id, "root-a");

    free(token);
    flowie_control_jwt_jwks_authenticator_destroy(authenticator);
  }

  it("rejects claim algorithm time and account-state mismatches") {
    flowie_control_jwt_jwks_authenticator_t *authenticator = jwt_test_authenticator();
    jwt_test_claims_t claims = jwt_test_valid_claims();
    flowie_control_external_auth_assertion_t assertion =
        FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
    char *token;
    flowie_control_external_auth_request_t request;
    check_equal(flowie_control_jwt_jwks_authenticator_install(authenticator, JWT_TEST_JWKS,
                                                              sizeof(JWT_TEST_JWKS) - 1u, 1200u),
                SALTS_OK);

    claims.issuer = "https://other.example";
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    claims.audience = "other";
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    claims.subject = "device-b";
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    claims.domain_id = "root-b";
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    claims.account_enabled = 0;
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    claims.issued_at = 1001u;
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    claims.not_before = 1031u;
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    claims.expires_at = 969u;
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    claims.revision = 0u;
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPROTO);
    free(token);
    claims = jwt_test_valid_claims();
    claims.assurance_level = 4u;
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPROTO);
    free(token);
    claims = jwt_test_valid_claims();
    claims.algorithm = alg_hs256;
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);

    flowie_control_jwt_jwks_authenticator_destroy(authenticator);
  }

  it("rejects stale snapshots wrong kids and modified signatures") {
    flowie_control_jwt_jwks_authenticator_t *authenticator = jwt_test_authenticator();
    jwt_test_claims_t claims = jwt_test_valid_claims();
    char *token = jwt_test_encode(&claims);
    flowie_control_external_auth_request_t request = jwt_test_request(token);
    flowie_control_external_auth_assertion_t assertion =
        FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
    char *signature;

    check_equal(flowie_control_jwt_jwks_authenticator_install(authenticator, JWT_TEST_JWKS,
                                                              sizeof(JWT_TEST_JWKS) - 1u, 1000u),
                SALTS_OK);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EBUSY);
    check_equal(flowie_control_jwt_jwks_authenticator_install(authenticator, JWT_TEST_JWKS,
                                                              sizeof(JWT_TEST_JWKS) - 1u, 1200u),
                SALTS_OK);
    free(token);
    claims.kid = "key-2";
    token = jwt_test_encode(&claims);
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    claims = jwt_test_valid_claims();
    token = jwt_test_encode(&claims);
    signature = strrchr(token, '.');
    check_not_null(signature);
    signature[1] = signature[1] == 'A' ? 'B' : 'A';
    request = jwt_test_request(token);
    check_equal(flowie_control_jwt_jwks_authenticator_verify_token(authenticator, &request, 1000u,
                                                                   &assertion),
                SALTS_EPERM);
    free(token);
    flowie_control_jwt_jwks_authenticator_destroy(authenticator);
  }

  it("rejects duplicate kids non-signing keys and private key material") {
    flowie_control_jwt_jwks_authenticator_t *authenticator = jwt_test_authenticator();
    static const char duplicate[] =
        "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed448\",\"kid\":\"key-1\","
        "\"use\":\"sig\",\"alg\":\"EdDSA\",\"x\":\"a\"},{\"kty\":\"OKP\","
        "\"crv\":\"Ed448\",\"kid\":\"key-1\",\"use\":\"sig\",\"alg\":\"EdDSA\","
        "\"x\":\"b\"}]}";
    static const char encryption[] =
        "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed448\",\"kid\":\"key-1\","
        "\"use\":\"enc\",\"alg\":\"EdDSA\",\"x\":\"a\"}]}";
    static const char private_key[] =
        "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed448\",\"kid\":\"key-1\","
        "\"use\":\"sig\",\"alg\":\"EdDSA\",\"x\":\"a\",\"d\":\"secret\"}]}";
    static const char wrong_algorithm[] =
        "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed448\",\"kid\":\"key-1\","
        "\"use\":\"sig\",\"alg\":\"RS256\",\"x\":\"a\"}]}";

    check_equal(flowie_control_jwt_jwks_authenticator_install(authenticator, duplicate,
                                                              sizeof(duplicate) - 1u, 1200u),
                SALTS_EPROTO);
    check_equal(flowie_control_jwt_jwks_authenticator_install(authenticator, encryption,
                                                              sizeof(encryption) - 1u, 1200u),
                SALTS_EPROTO);
    check_equal(flowie_control_jwt_jwks_authenticator_install(authenticator, private_key,
                                                              sizeof(private_key) - 1u, 1200u),
                SALTS_EPROTO);
    check_equal(flowie_control_jwt_jwks_authenticator_install(authenticator, wrong_algorithm,
                                                              sizeof(wrong_algorithm) - 1u, 1200u),
                SALTS_EPROTO);
    flowie_control_jwt_jwks_authenticator_destroy(authenticator);
  }
}
