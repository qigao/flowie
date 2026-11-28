#ifndef FLOWIE_RELEASE_NETWORK_FIXTURE_H
#define FLOWIE_RELEASE_NETWORK_FIXTURE_H

#include "flowie_control_release_http_internal.h"
#include "flowie_test_cnet.h"
#include "tls_test_pki.h"
#include "tinytest.h"

#include <core/error.h>
#include <core/zstd.h>
#include <crypto/sha256.h>
#include <crypto/sm2.h>
#include <crypto/sm2_cms.h>
#include <crypto/sm3.h>
#include <http_client/http.h>

#include <stdio.h>

static const char issuer_id[] = SALTS_CRYPTO_SM2_DEFAULT_ID;
static const char signer_id[] = "Flowie fixture signer";

static X509_KEY *test_release_generate_key(void) {
  int curve = OID_sm2;
  X509_KEY *key = calloc(1, sizeof(*key));
  if (key && x509_key_generate(key, OID_ec_public_key, &curve, sizeof(curve)) != 1) {
    tls_test_pki_free_key(key);
    return NULL;
  }
  return key;
}

static tls_test_certificate *test_release_certificate(X509_KEY *key,
    const tls_test_certificate *issuer, X509_KEY *issuer_key, int purpose) {
  return tls_test_pki_make_certificate_with_signature(key, issuer, issuer_key, issuer ? 2u : 1u,
      issuer ? "Flowie release signer" : "Flowie release root", !issuer, 0, purpose, -300L,
      TLS_TEST_PKI_VALID_SECONDS, OID_sm2sign_with_sm3, 1, issuer_id, sizeof(issuer_id) - 1u);
}

static int test_release_sign(const uint8_t *key, const salts_crypto_sm2_cert_span *cert,
                             const char *text, void *signature, size_t *length) {
  const salts_crypto_sm2_cms_sign_config config = {.format = SALTS_CRYPTO_SM2_CMS_RFC5652,
      .detached = true, .private_key = key, .signer_identity = signer_id,
      .signer_identity_size = sizeof(signer_id) - 1u, .certificates = cert, .certificate_count = 1u};
  return salts_crypto_sm2_cms_sign(&config, text, strlen(text), signature,
                                  SALTS_CRYPTO_SM2_CMS_MAX_DER, length);
}

enum { TEST_TAR_BLOCK = 512, TEST_TAR_CAPACITY = 8192 };
#define TEST_RELEASE_PACKAGE_NAME "device-a-rules"

static void test_tar_checksum(char *header) {
  memset(header + 148, ' ', 8);
  unsigned int sum = 0;
  for (size_t i = 0; i < TEST_TAR_BLOCK; ++i) sum += (unsigned char)header[i];
  (void)snprintf(header + 148, 7, "%06o", sum);
}

static size_t test_tar_add(char *tar, size_t used, const char *name, const char *prefix,
                           const void *data, size_t size) {
  const size_t padded = (size + TEST_TAR_BLOCK - 1u) / TEST_TAR_BLOCK * TEST_TAR_BLOCK;
  char *header = tar + used;
  memset(header, 0, TEST_TAR_BLOCK + padded);
  memcpy(header, name, strlen(name));
  if (prefix) memcpy(header + 345, prefix, strlen(prefix));
  (void)snprintf(header + 100, 8, "%07o", 0644u);
  memcpy(header + 108, "0000000", 7);
  memcpy(header + 116, "0000000", 7);
  (void)snprintf(header + 124, 12, "%011o", (unsigned int)size);
  memcpy(header + 136, "00000000000", 11);
  header[156] = '0';
  memcpy(header + 257, "ustar\0" "00", 8);
  test_tar_checksum(header);
  if (size) memcpy(header + TEST_TAR_BLOCK, data, size);
  return used + TEST_TAR_BLOCK + padded;
}

static size_t test_tar_manifest(char *tar, size_t used, const char *manifest_text,
                                const void *signature, size_t signature_size) {
  used = test_tar_add(tar, used, "manifest.json", NULL, manifest_text, strlen(manifest_text));
  used = test_tar_add(tar, used, "manifest.p7s", NULL, signature, signature_size);
  memset(tar + used, 0, 2u * TEST_TAR_BLOCK);
  return used + 2u * TEST_TAR_BLOCK;
}

static void test_release_hex(const uint8_t *data, char *hex) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < FLOWIE_RELEASE_DIGEST_BYTES; ++i) {
    hex[i * 2u] = digits[data[i] >> 4];
    hex[i * 2u + 1u] = digits[data[i] & 15u];
  }
  hex[FLOWIE_RELEASE_DIGEST_BYTES * 2u] = '\0';
}

typedef struct test_release_asset_s {
  const char *path;
  const void *bytes;
  size_t size;
} test_release_asset_t;

static int test_release_mqtt_package(const char *acl, const char *routes, const char *praktor,
    const uint8_t *base,
    const uint8_t *private_key, const salts_crypto_sm2_cert_span *certificate,
    const test_release_asset_t *asset, uint8_t *signature, char *tar, uint8_t *compressed,
    size_t *compressed_size) {
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  char acl_hex[FLOWIE_RELEASE_DIGEST_BYTES * 2u + 1u];
  char extra_hex[sizeof(acl_hex)], base_text[sizeof(acl_hex) + 2u];
  char extra_file[160] = "", extra_entrypoint[48] = "", extra_path[24] = "";
  char asset_file[384] = "";
  char text[768];
  const char *extra = routes ? routes : praktor;
  const size_t acl_size = strlen(acl), extra_size = extra ? strlen(extra) : 0u;
  if (salts_crypto_sm3(acl, acl_size, digest) != SALTS_CRYPTO_OK) return SALTS_EIO;
  test_release_hex(digest, acl_hex);
  if (extra) {
    const char *name = routes ? "routes" : "praktor";
    snprintf(extra_path, sizeof(extra_path), "%s.yml", name);
    snprintf(extra_entrypoint, sizeof(extra_entrypoint), ",\"%s\":\"%s\"", name, extra_path);
    if (salts_crypto_sm3(extra, extra_size, digest) != SALTS_CRYPTO_OK) return SALTS_EIO;
    test_release_hex(digest, extra_hex);
    snprintf(extra_file, sizeof(extra_file), ",{\"path\":\"%s\",\"size\":%zu,\"sm3\":\"%s\"}",
             extra_path, extra_size, extra_hex);
  }
  if (asset) {
    if (salts_crypto_sm3(asset->bytes, asset->size, digest) != SALTS_CRYPTO_OK) return SALTS_EIO;
    test_release_hex(digest, extra_hex);
    const int written = snprintf(asset_file, sizeof(asset_file),
        ",{\"path\":\"%s\",\"size\":%zu,\"sm3\":\"%s\"}", asset->path, asset->size, extra_hex);
    if (written < 0 || (size_t)written >= sizeof(asset_file)) return SALTS_ERANGE;
  }
  if (base) {
    base_text[0] = '"';
    test_release_hex(base, base_text + 1u);
    base_text[sizeof(acl_hex)] = '"';
    base_text[sizeof(acl_hex) + 1u] = '\0';
  } else {
    strcpy(base_text, "null");
  }
  const int length = snprintf(text, sizeof(text),
      "{\"name\":\"" TEST_RELEASE_PACKAGE_NAME "\",\"domain\":\"device-a\","
      "\"environment\":\"qa\",\"base_release\":%s,"
      "\"entrypoints\":{\"acl\":\"acl.yml\"%s},\"files\":["
      "{\"path\":\"acl.yml\",\"size\":%zu,\"sm3\":\"%s\"}%s%s]}",
      base_text, extra_entrypoint, acl_size, acl_hex, extra_file, asset_file);
  if (length <= 0 || (size_t)length >= sizeof(text)) return SALTS_EIO;
  size_t signature_size = 0;
  if (test_release_sign(private_key, certificate, text, signature, &signature_size) != SALTS_CRYPTO_OK)
    return SALTS_EIO;
  size_t used = test_tar_add(tar, 0, "acl.yml", NULL, acl, acl_size);
  if (extra) used = test_tar_add(tar, used, extra_path, NULL, extra, extra_size);
  if (asset) used = test_tar_add(tar, used, asset->path, NULL, asset->bytes, asset->size);
  used = test_tar_manifest(tar, used, text, signature, signature_size);
  return zstd_compress(compressed, TEST_TAR_CAPACITY * 2u, compressed_size, tar, used, 0);
}

static int test_release_mqtt_connect_session(flowie_test_cnet_client_t *client, const char *username,
    const char *password, uint8_t clean, uint8_t *session_present, uint8_t *reason) {
  static const uint8_t expiry[] = {0x11u, 0u, 0u, 0u, 60u};
  flowie_mqtt_connect_packet_t connect = FLOWIE_MQTT_CONNECT_PACKET_INIT;
  connect.version = FLOWIE_MQTT_VERSION_5;
  connect.clean_start = clean;
  if (session_present) connect.properties = (flowie_mqtt_span_t){expiry, sizeof(expiry)};
  connect.keep_alive = 30u;
  connect.client_id = (flowie_mqtt_span_t){(const uint8_t *)"release-client", 14u};
  connect.has_username = 1u;
  connect.username = (flowie_mqtt_span_t){(const uint8_t *)username, strlen(username)};
  if (password) {
    connect.has_password = 1u;
    connect.password = (flowie_mqtt_span_t){(const uint8_t *)password, strlen(password)};
  }
  uint8_t wire[128];
  size_t size = 0;
  if (flowie_mqtt_connect_packet_encode(&connect, wire, sizeof(wire), &size) != FLOWIE_MQTT_PARSE_OK)
    return SALTS_EPROTO;
  int rc = flowie_test_cnet_send(client, wire, size);
  if (rc != SALTS_OK) return rc;
  uint8_t header[2], body[64];
  rc = flowie_test_cnet_recv_exact(client, header, sizeof(header));
  if (rc != SALTS_OK) return rc;
  if (header[0] != 0x20 || header[1] < 3u || header[1] > sizeof(body)) return SALTS_EPROTO;
  rc = flowie_test_cnet_recv_exact(client, body, header[1]);
  if (rc != SALTS_OK) return rc;
  if (session_present) *session_present = body[0] & 1u;
  *reason = body[1];
  return SALTS_OK;
}

static int test_release_mqtt_connect_client(flowie_test_cnet_client_t *client, const char *username,
    const char *password, uint8_t *reason) {
  return test_release_mqtt_connect_session(client, username, password, 1u, NULL, reason);
}

static int test_release_mqtt_publish(flowie_test_cnet_client_t *client, uint8_t *reason) {
  const uint8_t wire[] = {0x32, 20, 0, 14, 'd','e','v','i','c','e','-','a','/','e','v','e','n','t',
                          0, 1, 0, 'x'};
  int rc = flowie_test_cnet_send(client, wire, sizeof(wire));
  if (rc != SALTS_OK) return rc;
  uint8_t header[2], body[16];
  rc = flowie_test_cnet_recv_exact(client, header, sizeof(header));
  if (rc != SALTS_OK) return rc;
  if (header[0] != 0x40 || header[1] < 2u || header[1] > sizeof(body)) return SALTS_EPROTO;
  rc = flowie_test_cnet_recv_exact(client, body, header[1]);
  if (rc != SALTS_OK) return rc;
  if (body[0] != 0 || body[1] != 1) return SALTS_EPROTO;
  *reason = header[1] >= 3u ? body[2] : 0;
  return SALTS_OK;
}

static int test_release_mqtt_subscribe(flowie_test_cnet_client_t *client, uint8_t *reason) {
  const uint8_t wire[] = {0x82, 20, 0, 2, 0, 0, 14,
                         'd','e','v','i','c','e','-','a','/','e','v','e','n','t',0};
  int rc = flowie_test_cnet_send(client, wire, sizeof(wire));
  if (rc != SALTS_OK) return rc;
  uint8_t response[6];
  rc = flowie_test_cnet_recv_exact(client, response, sizeof(response));
  if (rc != SALTS_OK) return rc;
  static const uint8_t expected[] = {0x90, 4, 0, 2, 0};
  if (memcmp(response, expected, sizeof(expected)) != 0) return SALTS_EPROTO;
  *reason = response[5];
  return SALTS_OK;
}

enum {
  TEST_RELEASE_TLS_CA, TEST_RELEASE_TLS_SERVER_CERT, TEST_RELEASE_TLS_SERVER_KEY,
  TEST_RELEASE_TLS_OPERATOR_CERT, TEST_RELEASE_TLS_OPERATOR_KEY,
  TEST_RELEASE_TLS_OTHER_CERT, TEST_RELEASE_TLS_OTHER_KEY, TEST_RELEASE_TLS_FILE_COUNT
};

typedef struct test_release_https_s {
  char *files[TEST_RELEASE_TLS_FILE_COUNT];
  flowie_control_release_http_t *server;
  chttp_client client;
  chttp_tls_profile operator_tls;
  chttp_tls_profile other_tls;
  chttp_response response;
  char operator_pin[65];
  char mqtt_pin[65];
  uint16_t port;
  char uri[48];
  char authority[32];
} test_release_https_t;

static int test_release_https_pki(test_release_https_t *http) {
  X509_KEY *keys[4] = {0};
  tls_test_certificate *certs[4] = {0};
  static const char *names[] = {"release TLS root", "localhost", "operator", "other"};
  uint8_t fingerprint[FLOWIE_RELEASE_DIGEST_BYTES];
  int result = SALTS_EIO;
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    keys[i] = tls_test_pki_generate_key();
    certs[i] = tls_test_pki_make_certificate(keys[i], i ? certs[0] : NULL, i ? keys[0] : NULL,
        (unsigned char)(i + 1u), names[i], i == 0, i == 1,
        i == 1 ? OID_kp_server_auth : OID_kp_client_auth, -300L, TLS_TEST_PKI_VALID_SECONDS);
    if (!certs[i]) goto done;
  }
  for (size_t i = 0; i < TEST_RELEASE_TLS_FILE_COUNT; ++i) {
    http->files[i] = tt_make_temp_file("flowie-release-https", ".pem");
    if (!http->files[i]) goto done;
  }
  if (tls_test_pki_write_certificate(http->files[TEST_RELEASE_TLS_CA], certs[0]) != 0)
    goto done;
  for (size_t i = 1; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    if (tls_test_pki_write_certificate(http->files[2u * i - 1u], certs[i]) != 0 ||
        tls_test_pki_write_key(http->files[2u * i], keys[i]) != 0) goto done;
  }
  if (salts_crypto_sha256(certs[2]->der, certs[2]->size, fingerprint) != SALTS_CRYPTO_OK) goto done;
  test_release_hex(fingerprint, http->operator_pin);
  if (salts_crypto_sha256(certs[3]->der, certs[3]->size, fingerprint) != SALTS_CRYPTO_OK) goto done;
  test_release_hex(fingerprint, http->mqtt_pin);
  result = SALTS_OK;
done:
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    free(certs[i]);
    tls_test_pki_free_key(keys[i]);
  }
  return result;
}

static int test_release_https_client_init(test_release_https_t *http) {
  snprintf(http->uri, sizeof(http->uri), "tls://127.0.0.1:%u", (unsigned)http->port);
  snprintf(http->authority, sizeof(http->authority), "localhost:%u", (unsigned)http->port);
  cnet_tls_client_config profile = {.size = sizeof(profile), .server_name = "localhost",
      .ca_file = http->files[TEST_RELEASE_TLS_CA],
      .cert_file = http->files[TEST_RELEASE_TLS_OPERATOR_CERT],
      .key_file = http->files[TEST_RELEASE_TLS_OPERATOR_KEY]};
  int rc = chttp_tls_profile_init(&http->operator_tls, &profile);
  if (rc != SALTS_OK) return rc;
  profile.cert_file = http->files[TEST_RELEASE_TLS_OTHER_CERT];
  profile.key_file = http->files[TEST_RELEASE_TLS_OTHER_KEY];
  rc = chttp_tls_profile_init(&http->other_tls, &profile);
  if (rc != SALTS_OK) return rc;
  const chttp_client_config client = {
      .network = {.backend =
#if defined(_WIN32)
          NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          NATIVE_IO_BACKEND_EPOLL,
#else
          NATIVE_IO_BACKEND_KQUEUE,
#endif
          .connection_capacity = 2, .command_capacity = 8, .request_capacity = 4,
          .completion_batch_capacity = 4, .event_capacity = 8, .receive_buffer_bytes = 65536,
          .max_send_bytes = 65536, .connect_timeout_ms = 2000, .read_timeout_ms = 3000,
          .write_timeout_ms = 3000, .tls_io_buffer_bytes = 65536, .tls_handshake_timeout_ms = 3000},
      .request_capacity = 2, .max_start_line_bytes = 1024, .max_header_count = 16,
      .max_header_bytes = 4096, .max_request_body_bytes = TEST_TAR_CAPACITY * 2u,
      .max_response_body_bytes = TEST_TAR_CAPACITY * 2u, .max_informational_responses = 4};
  return chttp_client_init(&http->client, &client);
}

static int test_release_https_start(test_release_https_t *http, flowie_control_release_mqtt_t *mqtt) {
  int rc = test_release_https_pki(http);
  if (rc != SALTS_OK) return rc;
  const cnet_tls_server_config tls = {.size = sizeof(tls),
      .cert_file = http->files[TEST_RELEASE_TLS_SERVER_CERT],
      .key_file = http->files[TEST_RELEASE_TLS_SERVER_KEY],
      .ca_file = http->files[TEST_RELEASE_TLS_CA], .client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED};
  const flowie_control_release_http_config_t server = {.host = "127.0.0.1", .tls = &tls, .mqtt = mqtt,
      .operator_id = "operator-a", .operator_certificate_sha256 = http->operator_pin,
      .signer_reference = "signer-a", .validation_policy_revision = "test-release-policy"};
  rc = flowie_control_release_http_open(&server, &http->server);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_release_http_port(http->server, &http->port);
  if (rc != SALTS_OK) return rc;
  return test_release_https_client_init(http);
}

static void test_release_https_close(test_release_https_t *http) {
  chttp_response_destroy(&http->response);
  if (http->client.impl) (void)chttp_client_destroy(&http->client, 5000);
  if (http->operator_tls.impl) (void)chttp_tls_profile_destroy(&http->operator_tls);
  if (http->other_tls.impl) (void)chttp_tls_profile_destroy(&http->other_tls);
  check_equal(flowie_control_release_http_close(&http->server), SALTS_OK);
  for (size_t i = 0; i < TEST_RELEASE_TLS_FILE_COUNT; ++i) {
    if (http->files[i]) remove(http->files[i]);
    free(http->files[i]);
  }
  *http = (test_release_https_t){0};
}

static int test_release_https_request(test_release_https_t *http, chttp_method method, const char *target,
    const chttp_header *headers, size_t header_count, const void *body, size_t body_size, int other) {
  chttp_response_destroy(&http->response);
  const chttp_options options = {.connection_uri = http->uri, .authority = http->authority,
      .target = target, .headers = headers, .header_count = header_count, .body = body,
      .body_size = body_size, .timeout_ms = 5000, .protocol = CHTTP_HTTP_1_1,
      .tls = other ? &http->other_tls : &http->operator_tls};
  chttp_error error = {0};
  int rc;
  if (method == CHTTP_METHOD_PUT) rc = chttp_put(&http->client, &options, &http->response, &error);
  else if (method == CHTTP_METHOD_POST) rc = chttp_post(&http->client, &options, &http->response, &error);
  else rc = chttp_get(&http->client, &options, &http->response, &error);
  if (rc != SALTS_OK)
    fprintf(stderr, "release HTTPS request %s failed: status=%d native=%d stage=%s\n",
        target, rc, error.native_status, error.stage ? error.stage : "unknown");
  return rc;
}

#endif
