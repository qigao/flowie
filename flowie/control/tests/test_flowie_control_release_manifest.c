#include "flowie_control_release_manifest_internal.h"
#include "flowie_control_release_archive_internal.h"
#include "flowie_control_release_store_internal.h"
#include "flowie_control_release_mqtt_internal.h"
#include "flowie_control_release_http_internal.h"
#include "flowie_control_test_turbodb.h"
#include "flowie_release_network_fixture.h"

#include <core/error.h>
#include <core/zstd.h>
#include <http_client/http.h>
#include <crypto/sha256.h>
#include <crypto/sm2.h>
#include <crypto/sm2_x509.h>
#include <crypto/sm3.h>

#include <stdio.h>

#define EMPTY_SM3 "1ab21d8355cfa17f8e61194831e81a8f22bec8c728fefb747ed035eb5082aa2b"
#define MANIFEST_ENTRIES "\"entrypoints\":{\"acl\":\"acl.yml\",\"routes\":\"routes.yml\"},\"files\":[" \
    "{\"path\":\"acl.yml\",\"size\":0,\"sm3\":\"" EMPTY_SM3 "\"}," \
    "{\"path\":\"routes.yml\",\"size\":0,\"sm3\":\"" EMPTY_SM3 "\"}]}"

static const char manifest[] = "{\"domain\":\"device-a\",\"environment\":\"qa\","
    "\"base_release\":null," MANIFEST_ENTRIES;

static int test_release_revoked(void *context, size_t depth,
    const salts_crypto_sm2_cert_span *certificate, const salts_crypto_sm2_cert_span *issuer,
    int64_t now) {
  unsigned int *calls = context;
  (void)depth;
  (void)certificate;
  (void)issuer;
  (void)now;
  ++*calls;
  return SALTS_CRYPTO_EVERIFY;
}

static int test_release_mqtt_identity(void *context, const flowie_security_auth_request_t *request,
                                      flowie_security_principal_t *out) {
  static const char credential[] = "fixture-password";
  (void)context;
  if (!request->identity || strcmp(request->identity, "writer") != 0 ||
      !request->secret || request->secret_size != sizeof(credential) - 1u ||
      memcmp(request->secret, credential, sizeof(credential) - 1u) != 0)
    return SALTS_EPERM;
  *out = (flowie_security_principal_t)FLOWIE_SECURITY_PRINCIPAL_INIT;
  strcpy(out->principal_id, "writer");
  strcpy(out->principal_type, "password");
  strcpy(out->domain_id, "device-a");
  strcpy(out->auth_method, "password");
  out->scope = FLOWIE_SECURITY_SCOPE_SELF;
  out->expires_at = (uint64_t)time(NULL) + 3600u;
  out->policy_version = 1u;
  return SALTS_OK;
}

static int test_release_mqtt_dispatch(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
                                       flowie_publish_result_t *out, void *context) {
  (void)endpoint;
  (void)message;
  (void)context;
  out->status = SALTS_OK;
  out->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_PROCESSED;
  return SALTS_OK;
}

/* Send only headers and observe the first response. A premature 100 Continue
 * would authorize a body before the application has checked this client. */
static int test_release_https_admission(test_release_https_t *http, int other,
                                        const char *signer, char status[12]) {
  const cnet_tls_client_config profile = {.size = sizeof(profile), .server_name = "localhost",
      .ca_file = http->files[TEST_RELEASE_TLS_CA],
      .cert_file = http->files[other ? TEST_RELEASE_TLS_OTHER_CERT : TEST_RELEASE_TLS_OPERATOR_CERT],
      .key_file = http->files[other ? TEST_RELEASE_TLS_OTHER_KEY : TEST_RELEASE_TLS_OPERATOR_KEY]};
  flowie_test_cnet_client_t *client = flowie_test_cnet_connect_configured(http->port, 0u, &profile);
  if (!client) return SALTS_EIO;
  char headers[320];
  const int size = snprintf(headers, sizeof(headers),
      "POST /domains/device-a/releases HTTP/1.1\r\nHost: %s\r\nContent-Length: 4096\r\n"
      "Content-Type: application/zstd\r\nFlowie-Release-Signer: %s\r\nExpect: 100-continue\r\n\r\n",
      http->authority, signer);
  int rc = flowie_test_cnet_send(client, (const uint8_t *)headers, (size_t)size);
  if (rc == SALTS_OK) rc = flowie_test_cnet_recv_exact(client, (uint8_t *)status, 12u);
  flowie_test_cnet_close(client);
  return rc;
}

spec("Flowie Control release manifest verification") {
  static X509_KEY *ca_key, *leaf_key;
  static tls_test_certificate *ca, *leaf;
  static uint8_t private_key[SALTS_CRYPTO_SM2_PRIVATE_KEY_SIZE];
  static uint8_t pin[SALTS_CRYPTO_SM2_PUBLIC_KEY_SIZE];
  static uint8_t signature[SALTS_CRYPTO_SM2_CMS_MAX_DER];
  static size_t signature_size;
  static salts_crypto_sm2_chain_entry chain;
  static salts_crypto_sm2_trust_policy trust;
  static salts_crypto_sm2_cms_verify_config signer;
  static flowie_release_manifest_t *result;
  static flowie_release_archive_t *archive;
  static flowie_control_release_store_t *release_store;
  static char *database_path;
  static void *stored_bytes;
  static flowie_control_release_mqtt_t *mqtt_runtime;
  static test_release_https_t https;
  static flowie_endpoint_core_t *endpoint;
  static flowie_test_cnet_client_t *mqtt_client;
  static char tar[TEST_TAR_CAPACITY];
  static uint8_t compressed[TEST_TAR_CAPACITY * 2u];

  before_each() {
    result = NULL;
    archive = NULL;
    release_store = NULL;
    database_path = NULL;
    stored_bytes = NULL;
    mqtt_runtime = NULL;
    endpoint = NULL;
    mqtt_client = NULL;
    signature_size = 0;
    ca = leaf = NULL;
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
    check_equal(salts_crypto_sm2_x509_matches_private_key(leaf->der, leaf->size, private_key),
                SALTS_CRYPTO_OK);
    chain = (salts_crypto_sm2_chain_entry){{leaf->der, leaf->size}, issuer_id, sizeof(issuer_id) - 1u};
    trust = (salts_crypto_sm2_trust_policy){.anchor = {ca->der, ca->size},
        .expected_leaf_public_key = pin, .validation_time = (int64_t)time(NULL),
        .purpose = SALTS_CRYPTO_SM2_TRUST_CODE_SIGNING,
        .revocation = SALTS_CRYPTO_SM2_REVOCATION_NOT_CHECKED};
    signer = (salts_crypto_sm2_cms_verify_config){.format = SALTS_CRYPTO_SM2_CMS_RFC5652,
        .detached = true, .signer_identity = signer_id, .signer_identity_size = sizeof(signer_id) - 1u,
        .chain = &chain, .chain_count = 1u, .trust = &trust};
  }

  after_each() {
    test_release_https_close(&https);
    flowie_test_cnet_close(mqtt_client);
    if (endpoint) {
      check_equal(flowie_endpoint_core_stop(endpoint), SALTS_OK);
      flowie_endpoint_core_destroy(endpoint);
    }
    flowie_control_release_mqtt_close(mqtt_runtime);
    flowie_control_release_archive_destroy(archive);
    flowie_control_release_store_close(release_store);
    if (database_path) {
      remove(database_path);
      free(database_path);
    }
    free(stored_bytes);
    free(result);
    free(leaf);
    free(ca);
    tls_test_pki_free_key(leaf_key);
    tls_test_pki_free_key(ca_key);
    gmssl_secure_clear(private_key, sizeof(private_key));
  }

  it("authenticates exact manifest bytes and resolves the ACL and route entries") {
    uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
    uint8_t issuer_public_key[SALTS_CRYPTO_SM2_PUBLIC_KEY_SIZE];
    check_equal(salts_crypto_sm2_x509_public_key(ca->der, ca->size, issuer_public_key),
                SALTS_CRYPTO_OK);
    check_equal(salts_crypto_sm2_x509_verify_signature(leaf->der, leaf->size, issuer_public_key,
        issuer_id, sizeof(issuer_id) - 1u), SALTS_CRYPTO_OK);
    check_equal(test_release_sign(private_key, &chain.certificate, manifest, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", manifest,
        sizeof(manifest) - 1u, signature, signature_size, &result), SALTS_OK);
    check_not_null(result);
    check_equal(result->file_count, 2u);
    check_equal(result->has_base_release, 0);
    check_equal(result->name, "");
    check_equal(result->files[result->acl_index].path, "acl.yml");
    check_equal(result->files[result->routes_index].path, "routes.yml");
    check_equal(result->files[0].size, 0u);
    check_equal(salts_crypto_sm3(NULL, 0, digest), SALTS_CRYPTO_OK);
    check_equal(result->files[0].sm3, digest, sizeof(digest));
    check_equal(salts_crypto_sm3(manifest, sizeof(manifest) - 1u, digest), SALTS_CRYPTO_OK);
    check_equal(result->sm3, digest, sizeof(digest));
  }

  it("requires the reserved RulesForge descriptor to be explicitly declared and signed") {
    static const struct { const char *entrypoint; const char *asset; int status; } cases[] = {
        {",\"rulesforge\":\"" FLOWIE_RELEASE_RULESFORGE_PATH "\"", FLOWIE_RELEASE_RULESFORGE_PATH, SALTS_OK},
        {"", FLOWIE_RELEASE_RULESFORGE_PATH, SALTS_EPROTO},
        {",\"rulesforge\":\"rules/alternate.json\"", "rules/alternate.json", SALTS_EPROTO},
        {",\"rulesforge\":\"" FLOWIE_RELEASE_RULESFORGE_PATH "\"", "rules/alternate.json", SALTS_EPROTO}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      char signed_text[640];
      const int written = snprintf(signed_text, sizeof(signed_text),
          "{\"domain\":\"device-a\",\"environment\":\"qa\",\"base_release\":null,"
          "\"entrypoints\":{\"acl\":\"acl.yml\"%s},\"files\":["
          "{\"path\":\"acl.yml\",\"size\":0,\"sm3\":\"" EMPTY_SM3 "\"},"
          "{\"path\":\"%s\",\"size\":1,\"sm3\":\"" EMPTY_SM3 "\"}]}",
          cases[i].entrypoint, cases[i].asset);
      check_true(written > 0 && (size_t)written < sizeof(signed_text));
      check_equal(test_release_sign(private_key, &chain.certificate, signed_text, signature,
          &signature_size), SALTS_CRYPTO_OK);
      check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", signed_text,
          (size_t)written, signature, signature_size, &result), cases[i].status);
      if (cases[i].status == SALTS_OK) {
        check_true(result->rulesforge_index < result->file_count);
        check_equal(result->files[result->rulesforge_index].path, FLOWIE_RELEASE_RULESFORGE_PATH);
        free(result);
        result = NULL;
      } else {
        check_null(result);
      }
    }
  }

  it("authenticates the package name and requires its Domain ACL") {
    static const char named[] = "{\"name\":\"device-a-rules\",\"domain\":\"device-a\","
        "\"environment\":\"qa\",\"base_release\":null," MANIFEST_ENTRIES;
    check_equal(test_release_sign(private_key, &chain.certificate, named, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", named,
        sizeof(named) - 1u, signature, signature_size, &result), SALTS_OK);
    check_equal(result->name, "device-a-rules");
    check_equal(result->files[result->acl_index].path, "acl.yml");
    free(result);
    result = NULL;
    char altered[sizeof(named)];
    memcpy(altered, named, sizeof(named));
    strstr(altered, "device-a-rules")[0] = 's';
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", altered,
        sizeof(named) - 1u, signature, signature_size, &result), SALTS_EPERM);
    check_null(result);

    static const char *const invalid[] = {"null", "\"\"", "\"../device-a\"", "\"device/a\"",
        "\"device\\u0000rules\"", "\"Device-a\"",
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"",
        "\"device-a-rules\",\"na\\u006de\":\"duplicate\""};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      char text[512];
      const int size = snprintf(text, sizeof(text), "{\"name\":%s,%s", invalid[i], manifest + 1u);
      check_true(size > 0 && (size_t)size < sizeof(text));
      check_equal(test_release_sign(private_key, &chain.certificate, text, signature,
                                    &signature_size), SALTS_CRYPTO_OK);
      check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", text,
          (size_t)size, signature, signature_size, &result), SALTS_EPROTO);
      check_null(result);
    }
    static const char no_acl[] = "{\"name\":\"device-a-rules\",\"domain\":\"device-a\","
        "\"environment\":\"qa\",\"base_release\":null,\"entrypoints\":{},\"files\":[]}";
    check_equal(test_release_sign(private_key, &chain.certificate, no_acl, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", no_acl,
        sizeof(no_acl) - 1u, signature, signature_size, &result), SALTS_EPROTO);
    check_null(result);
  }

  it("rejects modified bytes and a valid signature outside the expected Domain or environment") {
    char modified[sizeof(manifest)];
    memcpy(modified, manifest, sizeof(modified));
    modified[2] = 'X';
    check_equal(test_release_sign(private_key, &chain.certificate, manifest, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", modified,
        sizeof(modified) - 1u, signature, signature_size, &result), SALTS_EPERM);
    check_null(result);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-b", "qa", manifest,
        sizeof(manifest) - 1u, signature, signature_size, &result), SALTS_EPERM);
    check_null(result);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "prod", manifest,
        sizeof(manifest) - 1u, signature, signature_size, &result), SALTS_EPERM);
    check_null(result);
  }

  it("requires the exact actual file set, lengths and digests") {
    check_equal(test_release_sign(private_key, &chain.certificate, manifest, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", manifest,
        sizeof(manifest) - 1u, signature, signature_size, &result), SALTS_OK);
    flowie_release_manifest_file_t actual[2] = {{.path = "routes.yml"}, {.path = "acl.yml"}};
    for (size_t i = 0; i < 2u; ++i)
      check_equal(salts_crypto_sm3(NULL, 0, actual[i].sm3), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_check_files(result, actual, 2u), SALTS_OK);
    check_equal(flowie_control_release_manifest_check_files(result, actual, 1u), SALTS_EPROTO);
    actual[1].size = 1u;
    check_equal(flowie_control_release_manifest_check_files(result, actual, 2u), SALTS_EPROTO);
    actual[1].size = 0u;
    check_equal(salts_crypto_sm3("modified", 8u, actual[1].sm3), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_check_files(result, actual, 2u), SALTS_EPROTO);
    actual[1] = actual[0];
    check_equal(flowie_control_release_manifest_check_files(result, actual, 2u), SALTS_EPROTO);
  }

  it("requires a pinned, unrevoked, valid code-signing certificate") {
    unsigned int revocation_calls = 0;
    check_equal(test_release_sign(private_key, &chain.certificate, manifest, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    pin[1] ^= 1u;
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", manifest,
        sizeof(manifest) - 1u, signature, signature_size, &result), SALTS_EPERM);
    pin[1] ^= 1u;
    trust.revocation = SALTS_CRYPTO_SM2_REVOCATION_EXTERNAL;
    trust.check_revocation = test_release_revoked;
    trust.revocation_context = &revocation_calls;
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", manifest,
        sizeof(manifest) - 1u, signature, signature_size, &result), SALTS_EPERM);
    check_equal(revocation_calls, 1u);
    check_null(result);
    trust.validation_time += TLS_TEST_PKI_VALID_SECONDS + 60;
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", manifest,
        sizeof(manifest) - 1u, signature, signature_size, &result), SALTS_EPERM);
    check_equal(revocation_calls, 1u);
  }

  it("rejects an authenticated manifest with duplicate decoded keys") {
    static const char duplicate[] = "{\"domain\":\"device-a\",\"\\u0064omain\":\"device-a\","
        "\"environment\":\"qa\",\"base_release\":null," MANIFEST_ENTRIES;
    check_equal(test_release_sign(private_key, &chain.certificate, duplicate, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", duplicate,
        sizeof(duplicate) - 1u, signature, signature_size, &result), SALTS_EPROTO);
    check_null(result);
  }

  it("rejects a genuine signer certificate restricted to TLS server authentication") {
    free(leaf);
    leaf = test_release_certificate(leaf_key, ca, ca_key, OID_kp_server_auth);
    check_not_null(leaf);
    chain.certificate = (salts_crypto_sm2_cert_span){leaf->der, leaf->size};
    check_equal(test_release_sign(private_key, &chain.certificate, manifest, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", manifest,
        sizeof(manifest) - 1u, signature, signature_size, &result), SALTS_EPERM);
    check_null(result);
  }

  it("rejects authenticated traversal, reserved paths and out-of-range file sizes") {
    static const struct { const char *path; const char *size; int status; } cases[] = {
        {"../acl.yml", "0", SALTS_EPROTO}, {"aux/acl.yml", "0", SALTS_EPROTO},
        {"manifest.json", "0", SALTS_EPROTO}, {"routes.yml/nested", "0", SALTS_EPROTO},
        {"acl.yml", "8388609", SALTS_ENOSPC},
        {"acl.yml", "1e0", SALTS_EPROTO}};
    char text[512];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      const int size = snprintf(text, sizeof(text),
          "{\"domain\":\"device-a\",\"environment\":\"qa\",\"base_release\":null,"
          "\"entrypoints\":{\"acl\":\"%s\",\"routes\":\"routes.yml\"},\"files\":["
          "{\"path\":\"%s\",\"size\":%s,\"sm3\":\"" EMPTY_SM3 "\"},"
          "{\"path\":\"routes.yml\",\"size\":0,\"sm3\":\"" EMPTY_SM3 "\"}]}",
          cases[i].path, cases[i].path, cases[i].size);
      check_true(size > 0 && (size_t)size < sizeof(text));
      check_equal(test_release_sign(private_key, &chain.certificate, text, signature,
                                    &signature_size), SALTS_CRYPTO_OK);
      check_equal(flowie_control_release_manifest_verify(&signer, "device-a", "qa", text,
          (size_t)size, signature, signature_size, &result), cases[i].status);
      check_null(result);
    }
  }

  it("verifies a TAR.ZST release and borrows the original signed payload") {
    static const uint8_t acl[] = {0x00, 0xa5, 0x7f};
    char text[512], hex[FLOWIE_RELEASE_DIGEST_BYTES * 2u + 1u];
    uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
    check_equal(salts_crypto_sm3(acl, sizeof(acl), digest), SALTS_CRYPTO_OK);
    test_release_hex(digest, hex);
    const int written = snprintf(text, sizeof(text),
        "{\"domain\":\"device-a\",\"environment\":\"qa\",\"base_release\":null,"
        "\"entrypoints\":{\"acl\":\"config/acl.yml\",\"routes\":\"routes.yml\"},\"files\":["
        "{\"path\":\"config/acl.yml\",\"size\":3,\"sm3\":\"%s\"},"
        "{\"path\":\"routes.yml\",\"size\":0,\"sm3\":\"" EMPTY_SM3 "\"}]}", hex);
    check_true(written > 0 && (size_t)written < sizeof(text));
    check_equal(test_release_sign(private_key, &chain.certificate, text, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    size_t tar_size = test_tar_add(tar, 0, "acl.yml", "config", acl, sizeof(acl));
    tar_size = test_tar_add(tar, tar_size, "routes.yml", NULL, NULL, 0);
    tar_size = test_tar_manifest(tar, tar_size, text, signature, signature_size);
    size_t compressed_size = 0;
    check_equal(zstd_compress(compressed, sizeof(compressed), &compressed_size, tar, tar_size, 0),
                SALTS_OK);
    check_equal(flowie_control_release_archive_verify(&signer, "device-a", "qa", compressed,
        compressed_size, &archive), SALTS_OK);
    check_not_null(archive);
    const void *payload = NULL;
    size_t payload_size = 0;
    check_equal(flowie_control_release_archive_manifest(archive)->file_count, 2u);
    check_equal(flowie_control_release_archive_file(archive, "config/acl.yml", &payload,
        &payload_size), SALTS_OK);
    check_equal(payload_size, sizeof(acl));
    check_equal(payload, acl, sizeof(acl));
    check_equal(salts_crypto_sha256(compressed, compressed_size, digest), SALTS_CRYPTO_OK);
    check_equal(flowie_control_release_archive_digest(archive), digest, sizeof(digest));
    check_equal(flowie_control_release_archive_file(archive, "manifest.p7s", &payload,
        &payload_size), SALTS_ENOENT);
    flowie_control_release_archive_destroy(archive);
    archive = NULL;

    tar[TEST_TAR_BLOCK] = 1;
    check_equal(zstd_compress(compressed, sizeof(compressed), &compressed_size, tar, tar_size, 0),
                SALTS_OK);
    check_equal(flowie_control_release_archive_verify(&signer, "device-a", "qa", compressed,
        compressed_size, &archive), SALTS_EPROTO);
    check_null(archive);
  }

  it("rejects unsafe TAR entry types, names and truncated frame or end markers") {
    check_equal(test_release_sign(private_key, &chain.certificate, manifest, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    const size_t entries = test_tar_add(tar, TEST_TAR_BLOCK, "routes.yml", NULL, NULL, 0);
    const size_t tar_size = test_tar_manifest(tar, entries, manifest, signature, signature_size);
    const char *bad_names[] = {"../acl.yml", "acl.yml", "acl.yml", "acl.yml"};
    const char types[] = {'0', '2', '0', '0'};
    for (size_t i = 0; i < sizeof(types); ++i) {
      (void)test_tar_add(tar, 0, bad_names[i], NULL, NULL, 0);
      tar[156] = types[i];
      test_tar_checksum(tar);
      if (i == 2u) tar[100] = '7'; /* Malformed checksum. */
      const size_t length = i == 3u ? tar_size - TEST_TAR_BLOCK : tar_size;
      size_t compressed_size = 0;
      check_equal(zstd_compress(compressed, sizeof(compressed), &compressed_size, tar, length, 0),
                  SALTS_OK);
      check_equal(flowie_control_release_archive_verify(&signer, "device-a", "qa", compressed,
          compressed_size, &archive), SALTS_EPROTO);
      check_null(archive);
    }

    (void)test_tar_add(tar, 0, "acl.yml", NULL, NULL, 0);
    size_t compressed_size = 0;
    check_equal(zstd_compress(compressed, sizeof(compressed), &compressed_size, tar, tar_size, 0),
                SALTS_OK);
    check_equal(flowie_control_release_archive_verify(&signer, "device-a", "qa", compressed,
        compressed_size - 1u, &archive), SALTS_EPROTO);
    memcpy(compressed + compressed_size, compressed, compressed_size);
    check_equal(flowie_control_release_archive_verify(&signer, "device-a", "qa", compressed,
        compressed_size * 2u, &archive), SALTS_EPROTO);
    check_equal(flowie_control_release_archive_verify(&signer, "device-a", "qa", tar, tar_size,
        &archive), SALTS_EPROTO);
    check_null(archive);
  }

  it("rejects a frame whose declared TAR size exceeds the expansion limit") {
    /* Ordinary single-segment Zstd header declaring 33 MiB. Admission must reject the
     * size before scanning blocks or trying to allocate its output. */
    static const uint8_t oversized[] = {0x28, 0xb5, 0x2f, 0xfd, 0xa0, 0, 0, 0x10, 0x02};
    check_equal(flowie_control_release_archive_verify(&signer, "device-a", "qa", oversized,
        sizeof(oversized), &archive), SALTS_ENOSPC);
    check_null(archive);
  }

  it("persists a verified package, deduplicates it at quota and reads it after reopening") {
    static const char named[] = "{\"name\":\"" TEST_RELEASE_PACKAGE_NAME "\",\"domain\":\"device-a\","
        "\"environment\":\"qa\",\"base_release\":null," MANIFEST_ENTRIES;
    flowie_control_test_turbodb_t database;
    const flowie_control_release_store_limits_t limits = {
        .domain_count = 1, .domain_bytes = FLOWIE_RELEASE_MAX_COMPRESSED_BYTES,
        .total_count = 1, .total_bytes = FLOWIE_RELEASE_MAX_COMPRESSED_BYTES};
    database_path = tt_make_temp_file("flowie-release-store", ".sqlite3");
    check_not_null(database_path);
    check_equal(flowie_control_test_turbodb_init(&database, database_path), 0);
    check_equal(flowie_control_release_store_open(&database.config, &limits, &release_store), SALTS_OK);
    check_equal(test_release_sign(private_key, &chain.certificate, named, signature,
                                  &signature_size), SALTS_CRYPTO_OK);
    size_t tar_size = test_tar_add(tar, 0, "acl.yml", NULL, NULL, 0);
    tar_size = test_tar_add(tar, tar_size, "routes.yml", NULL, NULL, 0);
    tar_size = test_tar_manifest(tar, tar_size, named, signature, signature_size);
    size_t compressed_size = 0;
    check_equal(zstd_compress(compressed, sizeof(compressed), &compressed_size, tar, tar_size, 0),
                SALTS_OK);
    flowie_control_release_receipt_t first, replay;
    check_equal(flowie_control_release_store_admit(release_store, "prod", "device-a", "signer-a",
        &signer, compressed, compressed_size, &first), SALTS_EPERM);
    check_equal(flowie_control_release_store_admit(release_store, "qa", "device-a", "signer-a",
        &signer, compressed, compressed_size, &first), SALTS_OK);
    check_equal(first.created, 1);
    check_equal(first.name, TEST_RELEASE_PACKAGE_NAME);
    check_equal(flowie_control_release_store_admit(release_store, "qa", "device-a", "signer-a",
        &signer, compressed, compressed_size, &replay), SALTS_OK);
    check_equal(replay.created, 0);
    check_equal(first.digest, replay.digest, sizeof(first.digest));

    flowie_control_release_store_close(release_store);
    release_store = NULL;
    check_equal(flowie_control_release_store_open(&database.config, &limits, &release_store), SALTS_OK);
    size_t length = 0;
    check_equal(flowie_control_release_store_read(release_store, "qa", "device-b", first.digest,
        &stored_bytes, &length), SALTS_ENOENT);
    check_equal(flowie_control_release_store_read(release_store, "qa", "device-a", first.digest,
        &stored_bytes, &length), SALTS_OK);
    check_equal(length, compressed_size);
    check_equal(stored_bytes, compressed, length);

    check_equal(flowie_control_release_archive_verify(&signer, "device-a", "qa", stored_bytes,
        length, &archive), SALTS_OK);
    const flowie_release_manifest_t *verified = flowie_control_release_archive_manifest(archive);
    const uint8_t *acl_digest = verified->files[verified->acl_index].sm3;
    flowie_control_release_active_t active;
    int replayed = 0;
    check_equal(flowie_control_release_store_current(release_store, "qa", "device-a", &active),
                SALTS_OK);
    check_equal(active.generation, 0u);
    check_equal(flowie_control_release_store_commit(release_store, "qa", "device-a", "operator-a",
        "activate-a", 1u, "test-policy", archive, acl_digest, &active, &replayed), SALTS_ECANCELED);
    check_equal(flowie_control_release_store_validation(release_store, "qa", "device-a",
        first.digest, 0, "test-policy"), SALTS_OK);
    check_equal(flowie_control_release_store_commit(release_store, "qa", "device-a", "operator-a",
        "activate-a", 0u, "other-policy", archive, acl_digest, &active, &replayed), SALTS_EAUTH);
    check_equal(flowie_control_release_store_commit(release_store, "qa", "device-a", "operator-a",
        "activate-a", 0u, "test-policy", archive, acl_digest, &active, &replayed), SALTS_OK);
    check_equal(active.generation, 1u);
    check_equal(active.acl_version, 1u);
    check_equal(active.digest, first.digest, sizeof(first.digest));
    check_equal(replayed, 0);
    flowie_control_release_store_close(release_store);
    release_store = NULL;
    check_equal(flowie_control_release_store_open(&database.config, &limits, &release_store), SALTS_OK);
    check_equal(flowie_control_release_store_commit(release_store, "qa", "device-a", "operator-a",
        "activate-a", 0u, "test-policy", archive, acl_digest, &active, &replayed), SALTS_OK);
    check_equal(replayed, 1);
    check_equal(active.generation, 1u);
    check_equal(flowie_control_release_store_commit(release_store, "qa", "device-a", "operator-a",
        "activate-a", 1u, "test-policy", archive, acl_digest, &active, &replayed), SALTS_EALREADY);
    check_equal(flowie_control_release_store_current(release_store, "qa", "device-a", &active),
                SALTS_OK);
    check_equal(active.generation, 1u);

    memset(tar + tar_size, 0, TEST_TAR_BLOCK);
    check_equal(zstd_compress(compressed, sizeof(compressed), &compressed_size, tar,
        tar_size + TEST_TAR_BLOCK, 0), SALTS_OK);
    check_equal(flowie_control_release_store_admit(release_store, "qa", "device-a", "signer-a",
        &signer, compressed, compressed_size, &replay), SALTS_ENOSPC);
    check_equal(replay.created, 0);
  }

  it("recovers unstarted rule events and retains interrupted delivery records") {
    flowie_control_test_turbodb_t database;
    database_path = tt_make_temp_file("flowie-rule-journal", ".sqlite3");
    check_not_null(database_path);
    check_equal(flowie_control_test_turbodb_init(&database, database_path), 0);
    check_equal(flowie_control_release_journal_open(&database.config, &release_store), SALTS_OK);
    check_equal(flowie_control_release_store_check_journal(release_store, NULL, NULL), SALTS_OK);
    check_equal(flowie_control_release_store_check_journal(release_store, release_store, &database.config), SALTS_EINVAL);
    const uint8_t digest[32] = {1};
    const flowie_release_journal_scope_t scope = {"qa", "device-a", 1u, digest};
    const uint8_t exchange[SALTS_UUID_SIZE] = {[6] = 0x40};
    check_equal(flowie_control_release_enqueue(release_store, &scope, "writer", "device-a/event", "a", 1u, exchange), SALTS_OK);
    check_equal(flowie_control_release_enqueue(release_store, &scope, "writer", "device-a/event", "a", 1u, exchange), SALTS_OK);
    check_equal(flowie_control_release_enqueue(release_store, &scope, "writer", "device-a/event", "b", 1u, NULL), SALTS_OK);
    flowie_release_job_t job;
    check_equal(flowie_control_release_claim(release_store, &scope, &job), SALTS_OK);
    check_equal(job.sequence, 1);
    check_equal(flowie_control_release_record_output(release_store, &scope, job.sequence,
        "interrupted-output", "alerts", "intent", 6u), SALTS_OK);
    flowie_control_release_store_close(release_store);
    release_store = NULL;
    check_equal(flowie_control_release_journal_open(&database.config, &release_store), SALTS_OK);
    const uint8_t wrong_digest[32] = {2};
    const flowie_release_journal_scope_t wrong_release = {"qa", "device-a", 1u, wrong_digest};
    const flowie_release_journal_scope_t restored_older = {"qa", "device-a", 0u, wrong_digest};
    check_equal(flowie_control_release_journal_verify(release_store, &wrong_release), SALTS_EPROTO);
    check_equal(flowie_control_release_journal_verify(release_store, &restored_older), SALTS_EPROTO);
    check_equal(flowie_control_release_journal_verify(release_store, &scope), SALTS_OK);
    check_equal(flowie_control_release_interrupt(release_store, &scope), SALTS_OK);
    check_equal(flowie_control_release_claim(release_store, &scope, &job), SALTS_OK);
    check_equal(job.sequence, 2);
    check_equal(job.size, 1u);
    check_equal(job.payload[0], 'b');
    check_equal(flowie_control_release_finish(release_store, &scope, job.sequence, SALTS_OK), SALTS_OK);
    int pending = 1;
    check_equal(flowie_control_release_pending(release_store, &scope, &pending), SALTS_OK);
    check_equal(pending, 0);
    check_equal(flowie_control_release_finish_output(release_store, "interrupted-output", SALTS_OK, 202u),
        SALTS_EIO);
  }

  it("rotates settled history at capacity and keeps uncertain deliveries") {
    flowie_control_test_turbodb_t database;
    database_path = tt_make_temp_file("flowie-rule-history", ".sqlite3");
    check_not_null(database_path);
    check_equal(flowie_control_test_turbodb_init(&database, database_path), 0);
    check_equal(flowie_control_release_journal_open(&database.config, &release_store), SALTS_OK);
    const uint8_t digest[32] = {1};
    const flowie_release_journal_scope_t scope = {"qa", "device-a", 1u, digest};
    check_equal(flowie_control_release_enqueue(release_store, &scope, "writer", "device-a/event", "a", 1u, NULL), SALTS_OK);
    flowie_release_job_t job;
    check_equal(flowie_control_release_claim(release_store, &scope, &job), SALTS_OK);
    check_equal(flowie_control_release_record_output(release_store, &scope, job.sequence,
        "unknown", "alerts", "intent", 6u), SALTS_OK);
    check_equal(flowie_control_release_interrupt(release_store, &scope), SALTS_OK);

    flowie_control_database_t *db = NULL;
    check_equal(flowie_control_test_database_open(database_path, &db), FLOWIE_CONTROL_DB_OK);
    const int seeded = flowie_control_database_exec(db,
        "WITH RECURSIVE ids(id) AS (SELECT 2 UNION ALL SELECT id+1 FROM ids WHERE id<8192) "
        "INSERT INTO flowie_rule_event SELECT 'qa','device-a',1,id,'writer','device-a/event',X'61',"
        "unixepoch()+10,unixepoch()+10,unixepoch()+10,0 FROM ids;"
        "INSERT INTO flowie_rule_output SELECT CAST(sequence AS TEXT),environment,domain_id,"
        "generation,sequence,'alerts',X'61',received_at,finished_at,0,202 FROM flowie_rule_event "
        "WHERE sequence>1;"
        "UPDATE flowie_rule_generation SET sequence=8192 WHERE environment='qa' AND domain_id='device-a';",
        NULL, NULL, NULL);
    check_equal(flowie_control_database_close(db), FLOWIE_CONTROL_DB_OK);
    check_equal(seeded, FLOWIE_CONTROL_DB_OK);
    check_equal(flowie_control_release_enqueue(release_store, &scope, "writer", "device-a/event", "z", 1u, NULL), SALTS_OK);
    check_equal(flowie_control_release_claim(release_store, &scope, &job), SALTS_OK);
    check_equal(job.sequence, 8193);
    check_equal(flowie_control_release_record_output(release_store, &scope, job.sequence,
        "new-1", "alerts", "z", 1u), SALTS_OK);
    check_equal(flowie_control_release_record_output(release_store, &scope, job.sequence,
        "new-2", "alerts", "z", 1u), SALTS_OK);

    check_equal(flowie_control_test_database_open(database_path, &db), FLOWIE_CONTROL_DB_OK);
    flowie_control_statement_t *stmt = NULL;
    int status = flowie_control_database_prepare(db,
        "SELECT (SELECT count(*) FROM flowie_rule_event), (SELECT count(*) FROM flowie_rule_output), "
        "(SELECT count(*) FROM flowie_rule_output WHERE output_key='unknown'), "
        "(SELECT count(*) FROM flowie_rule_event WHERE sequence=2), "
        "(SELECT count(*) FROM flowie_rule_output WHERE output_key='3')",
        -1, &stmt, NULL);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
    int counts[5] = {0};
    if (status == FLOWIE_CONTROL_DB_ROW)
      for (int i = 0; i < 5; ++i) counts[i] = flowie_control_database_column_int(stmt, i);
    flowie_control_database_finalize(stmt);
    check_equal(flowie_control_database_close(db), FLOWIE_CONTROL_DB_OK);
    check_equal(status, FLOWIE_CONTROL_DB_ROW);
    check_equal(counts[0], 8192);
    check_equal(counts[1], 8192);
    check_equal(counts[2], 1);
    check_equal(counts[3], 0);
    check_equal(counts[4], 0);
  }

  it("validates and activates signed releases over mTLS while an MQTT 5 listener runs") {
    static const char denied[] = "user writer allow {\n  deny write topic device-a/event\n}";
    static const char allowed[] = "user writer allow {\n  readwrite topic device-a/event\n}";
    static const flowie_security_auth_provider_t identities = {
        sizeof(identities), NULL, test_release_mqtt_identity};
    const flowie_control_release_store_limits_t limits = {
        .domain_count = 4, .domain_bytes = FLOWIE_RELEASE_MAX_COMPRESSED_BYTES,
        .total_count = 4, .total_bytes = FLOWIE_RELEASE_MAX_COMPRESSED_BYTES};
    flowie_control_test_turbodb_t database;
    database_path = tt_make_temp_file("flowie-release-mqtt", ".sqlite3");
    check_not_null(database_path);
    check_equal(flowie_control_test_turbodb_init(&database, database_path), 0);
    check_equal(flowie_control_release_store_open(&database.config, &limits, &release_store), SALTS_OK);
    const flowie_control_release_mqtt_config_t deployment = {.store = release_store,
        .environment = "qa", .domain = "device-a", .realm_name = "security.domain-release",
        .resource_uid = "security.domain-release", .owner_name = "flowie-release-mqtt",
        .auth_method = "password", .identity_provider = &identities, .signer = &signer};
    check_equal(flowie_control_release_mqtt_open(&deployment, &mqtt_runtime), SALTS_OK);
    check_equal(test_release_https_start(&https, mqtt_runtime), SALTS_OK);

    flowie_control_release_receipt_t baseline, replacement;
    flowie_control_release_active_t active;
    int replayed = 0;
    size_t compressed_size = 0;
    check_equal(test_release_mqtt_package(denied, NULL, NULL, NULL, private_key, &chain.certificate,
        NULL, signature, tar, compressed, &compressed_size), SALTS_OK);
    check_equal(salts_crypto_sha256(compressed, compressed_size, baseline.digest), SALTS_CRYPTO_OK);
    static const char releases[] = "/domains/device-a/releases";
    static const char active_route[] = "/domains/device-a/active-release";
    static const char status_route[] = "/domains/device-a/status";
    static const chttp_header upload[] = {
        {"Content-Type", "application/zstd"}, {"Flowie-Release-Signer", "signer-a"}};
    static const chttp_header initial[] = {{"Content-Type", "application/json"},
        {"If-None-Match", "*"}, {"Idempotency-Key", "activate-1"}};
    static const chttp_header replace[] = {{"Content-Type", "application/json"},
        {"If-Match", "\"release-1\""}, {"Idempotency-Key", "activate-2"}};
    static const chttp_header stale[] = {{"Content-Type", "application/json"},
        {"If-None-Match", "*"}, {"Idempotency-Key", "stale-activation"}};
    check_equal(test_release_sign(private_key, &chain.certificate, manifest, signature,
        &signature_size), SALTS_CRYPTO_OK);
    size_t unnamed_size = test_tar_add(tar, 0, "acl.yml", NULL, NULL, 0);
    unnamed_size = test_tar_add(tar, unnamed_size, "routes.yml", NULL, NULL, 0);
    unnamed_size = test_tar_manifest(tar, unnamed_size, manifest, signature, signature_size);
    static uint8_t unnamed_compressed[TEST_TAR_CAPACITY * 2u];
    size_t unnamed_compressed_size = 0;
    check_equal(zstd_compress(unnamed_compressed, sizeof(unnamed_compressed),
        &unnamed_compressed_size, tar, unnamed_size, 0), SALTS_OK);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, releases, upload, 2,
        unnamed_compressed, unnamed_compressed_size, 0), SALTS_OK);
    check_equal(https.response.status_code, 422u);
    check_not_null(strstr(https.response.body, "package_name_required"));
    char hex[65], download[112], validate[128], acl_preview[128], body[96], baseline_body[96], after[128];
    test_release_hex(baseline.digest, hex);
    snprintf(download, sizeof(download), "%s/%s", releases, hex);
    snprintf(validate, sizeof(validate), "%s/validate", download);
    snprintf(acl_preview, sizeof(acl_preview), "%s/acl", download);
    snprintf(baseline_body, sizeof(baseline_body), "{\"digest\":\"%s\"}", hex);

    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, "/healthz", NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, active_route, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 404u);
    static const char page[] = "/domains/device-a/packages";
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, page, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "Content-Type"), "text/html; charset=utf-8");
    check_not_null(chttp_response_header(&https.response, "Content-Security-Policy"));
    check_not_null(strstr(https.response.body, "Domain Packages"));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, page, NULL, 0, NULL, 0, 1), SALTS_OK);
    check_equal(https.response.status_code, 403u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, "/domains/other/packages",
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 404u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, "/domains/device-a/packages/app.js",
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "Content-Type"), "text/javascript; charset=utf-8");
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, "/domains/device-a/packages/app.css",
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "Content-Type"), "text/css; charset=utf-8");
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, "/domains/device-a/packages/context",
        NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "Flowie-Release-Signer"), "signer-a");
    check_not_null(strstr(https.response.body, "\"domain\":\"device-a\""));
    uint8_t signer_fingerprint[FLOWIE_RELEASE_DIGEST_BYTES];
    char signer_sha256[65], certificate_field[128];
    check_equal(salts_crypto_sha256(chain.certificate.der, chain.certificate.size,
        signer_fingerprint), SALTS_CRYPTO_OK);
    test_release_hex(signer_fingerprint, signer_sha256);
    snprintf(certificate_field, sizeof(certificate_field),
        "\"signer_certificate_sha256\":\"%s\"", signer_sha256);
    check_not_null(strstr(https.response.body, certificate_field));
    snprintf(certificate_field, sizeof(certificate_field),
        "\"operator_certificate_sha256\":\"%s\"", https.operator_pin);
    check_not_null(strstr(https.response.body, certificate_field));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, "/domains/device-a/packages/context",
        NULL, 0, NULL, 0, 1), SALTS_OK);
    check_equal(https.response.status_code, 403u);
    char admission_status[12];
    check_equal(test_release_https_admission(&https, 1, "signer-a", admission_status), SALTS_OK);
    check_equal(admission_status, "HTTP/1.1 403", sizeof(admission_status));
    check_equal(test_release_https_admission(&https, 0, "untrusted", admission_status), SALTS_OK);
    check_equal(admission_status, "HTTP/1.1 403", sizeof(admission_status));
    check_equal(test_release_https_admission(&https, 0, "signer-a", admission_status), SALTS_OK);
    check_equal(admission_status, "HTTP/1.1 100", sizeof(admission_status));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, releases, upload, 2,
        compressed, compressed_size, 1), SALTS_OK);
    check_equal(https.response.status_code, 403u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, "/domains/other/releases", upload, 2,
        compressed, compressed_size, 0), SALTS_OK);
    check_equal(https.response.status_code, 404u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, download, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 404u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, releases, upload, 2,
        compressed, compressed_size, 0), SALTS_OK);
    check_equal(https.response.status_code, 201u);
    check_equal(chttp_response_header(&https.response, "Location"), download);
    check_not_null(strstr(https.response.body, "\"name\":\"" TEST_RELEASE_PACKAGE_NAME "\""));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, acl_preview, NULL, 0, NULL, 0, 1), SALTS_OK);
    check_equal(https.response.status_code, 403u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, acl_preview, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "Content-Type"), "text/plain; charset=utf-8");
    check_equal(chttp_response_header(&https.response, "Flowie-Package-Name"), TEST_RELEASE_PACKAGE_NAME);
    check_equal(chttp_response_header(&https.response, "Flowie-ACL-Path"), "acl.yml");
    check_equal(https.response.body_size, strlen(denied));
    check_equal(https.response.body, denied, strlen(denied));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, releases, upload, 2,
        compressed, compressed_size, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_not_null(strstr(https.response.body, "\"name\":\"" TEST_RELEASE_PACKAGE_NAME "\""));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, download, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(https.response.body_size, compressed_size);
    check_equal(https.response.body, compressed, compressed_size);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, releases, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_not_null(strstr(https.response.body, hex));
    snprintf(after, sizeof(after), "%s?after=%s", releases, hex);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, after, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_not_null(strstr(https.response.body, "\"releases\":[]"));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, initial, 1,
        baseline_body, strlen(baseline_body), 0), SALTS_OK);
    check_equal(https.response.status_code, 428u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, initial, 3,
        baseline_body, strlen(baseline_body), 0), SALTS_OK);
    check_equal(https.response.status_code, 409u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, validate, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_not_null(strstr(https.response.body, "\"base_generation\":0"));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, initial, 3,
        baseline_body, strlen(baseline_body), 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "ETag"), "\"release-1\"");

    const uint16_t port = flowie_test_cnet_port();
    check_true(port != 0);
    flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
    config.transport = FLOWIE_TRANSPORT_TCP;
    config.host = "127.0.0.1";
    config.port = port;
    config.max_connections = 4u;
    config.manage_sessions = 1;
    config.max_sessions = 4u;
    config.max_subscriptions_per_session = 4u;
    config.max_inflight_per_session = 4u;
    config.recv_timeout_ms = FLOWIE_TEST_CNET_TIMEOUT_MS;
    flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
    options.on_message = test_release_mqtt_dispatch;
    flowie_execution_binding_t execution = FLOWIE_EXECUTION_BINDING_INIT;
    flowie_endpoint_bindings_t bindings = FLOWIE_ENDPOINT_BINDINGS_INIT;
    bindings.security = flowie_control_release_mqtt_binding(mqtt_runtime);
    check_equal(flowie_endpoint_core_create_ex("release-mqtt-fixture", &config, &options,
        &execution, &bindings, &endpoint), SALTS_OK);
    check_equal(flowie_endpoint_core_start(endpoint), SALTS_OK);
    mqtt_client = flowie_test_cnet_connect(port);
    check_not_null(mqtt_client);
    uint8_t reason = 0xff;
    check_equal(test_release_mqtt_connect_client(mqtt_client, "writer", "fixture-password", &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(test_release_mqtt_publish(mqtt_client, &reason), SALTS_OK);
    check_equal(reason, 0x87u);
    check_equal(test_release_mqtt_subscribe(mqtt_client, &reason), SALTS_OK);
    check_equal(reason, 0x87u);

    check_equal(test_release_mqtt_package(allowed, NULL, NULL, baseline.digest, private_key,
        &chain.certificate, NULL, signature, tar, compressed, &compressed_size), SALTS_OK);
    check_equal(salts_crypto_sha256(compressed, compressed_size, replacement.digest), SALTS_CRYPTO_OK);
    test_release_hex(replacement.digest, hex);
    snprintf(validate, sizeof(validate), "%s/%s/validate", releases, hex);
    snprintf(body, sizeof(body), "{\"digest\":\"%s\"}", hex);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, releases, upload, 2,
        compressed, compressed_size, 0), SALTS_OK);
    check_equal(https.response.status_code, 201u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, validate, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_not_null(strstr(https.response.body, "\"base_generation\":1"));
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, stale, 3,
        body, strlen(body), 0), SALTS_OK);
    check_equal(https.response.status_code, 412u);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, replace, 3,
        body, strlen(body), 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "ETag"), "\"release-2\"");
    check_equal(chttp_response_header(&https.response, "Flowie-Runtime-Ready"), "true");
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, status_route, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_not_null(strstr(https.response.body, "\"observed_generation\":2,\"ready\":true"));
    check_equal(flowie_control_release_store_current(release_store, "qa", "device-a", &active), SALTS_OK);
    check_equal(active.generation, 2u);
    check_equal(active.acl_version, 2u);
    check_equal(test_release_mqtt_publish(mqtt_client, &reason), SALTS_OK);
    check_equal(reason, 0x87u);
    flowie_test_cnet_close(mqtt_client);
    mqtt_client = flowie_test_cnet_connect(port);
    check_not_null(mqtt_client);
    check_equal(test_release_mqtt_connect_client(mqtt_client, "writer", "fixture-password", &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(test_release_mqtt_publish(mqtt_client, &reason), SALTS_OK);
    check_equal(reason, 0u);
    check_equal(test_release_mqtt_subscribe(mqtt_client, &reason), SALTS_OK);
    check_equal(reason, 0u);

    check_equal(test_release_https_request(&https, CHTTP_METHOD_PUT, active_route, initial, 3,
        baseline_body, strlen(baseline_body), 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "ETag"), "\"release-1\"");
    check_equal(chttp_response_header(&https.response, "Idempotency-Replayed"), "true");
    check_equal(test_release_https_request(&https, CHTTP_METHOD_GET, active_route, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 200u);
    check_equal(chttp_response_header(&https.response, "ETag"), "\"release-2\"");
    check_equal(test_release_mqtt_publish(mqtt_client, &reason), SALTS_OK);
    check_equal(reason, 0u);

    flowie_control_release_receipt_t unsupported;
    check_equal(test_release_mqtt_package(denied, "routes: [external-workflow]\n", NULL, replacement.digest,
        private_key, &chain.certificate, NULL, signature, tar, compressed, &compressed_size), SALTS_OK);
    check_equal(flowie_control_release_store_admit(release_store, "qa", "device-a", "signer-a",
        &signer, compressed, compressed_size, &unsupported), SALTS_OK);
    test_release_hex(unsupported.digest, hex);
    snprintf(validate, sizeof(validate), "%s/%s/validate", releases, hex);
    check_equal(test_release_https_request(&https, CHTTP_METHOD_POST, validate, NULL, 0, NULL, 0, 0), SALTS_OK);
    check_equal(https.response.status_code, 422u);
    check_equal(flowie_control_release_store_current(release_store, "qa", "device-a", &active), SALTS_OK);
    check_equal(active.generation, 2u);

    flowie_control_release_receipt_t repacked;
    check_equal(test_release_mqtt_package(allowed, "routes: [ ]\n", NULL, replacement.digest,
        private_key, &chain.certificate, NULL, signature, tar, compressed, &compressed_size), SALTS_OK);
    check_equal(flowie_control_release_store_admit(release_store, "qa", "device-a", "signer-a",
        &signer, compressed, compressed_size, &repacked), SALTS_OK);
    uint64_t base_generation = 0;
    int acl_changed = -1;
    check_equal(flowie_control_release_mqtt_validate(mqtt_runtime, repacked.digest, "test-release-policy",
        &base_generation, &acl_changed), SALTS_OK);
    check_equal(base_generation, 2u);
    check_equal(acl_changed, 0);
    check_equal(flowie_control_release_mqtt_activate(mqtt_runtime, repacked.digest, "operator-a",
        "activate-4", 2u, "test-release-policy", &active, &replayed), SALTS_OK);
    check_equal(active.generation, 3u);
    check_equal(active.acl_version, 2u);
    check_equal(test_release_mqtt_publish(mqtt_client, &reason), SALTS_OK);
    check_equal(reason, 0u);

    flowie_test_cnet_close(mqtt_client);
    mqtt_client = NULL;
    check_equal(flowie_endpoint_core_stop(endpoint), SALTS_OK);
    flowie_endpoint_core_destroy(endpoint);
    endpoint = NULL;
    test_release_https_close(&https);
    flowie_control_release_mqtt_close(mqtt_runtime);
    mqtt_runtime = NULL;
    check_equal(flowie_control_release_mqtt_open(&deployment, &mqtt_runtime), SALTS_OK);
    const flowie_endpoint_security_binding_t *restored = flowie_control_release_mqtt_binding(mqtt_runtime);
    flowie_security_auth_request_t request = FLOWIE_SECURITY_AUTH_REQUEST_INIT;
    request.identity = "writer";
    request.method = "password";
    request.secret = (const uint8_t *)"fixture-password";
    request.secret_size = 16u;
    request.protocol = "mqtt5";
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    check_equal(flowie_security_authenticate(restored->auth_provider, &request, &principal), SALTS_OK);
    check_equal(principal.policy_version, 2u);
  }
}
