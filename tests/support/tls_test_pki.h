#ifndef FLOWIE_TLS_TEST_PKI_H
#define FLOWIE_TLS_TEST_PKI_H

#include "tls_test_support.h"
#include <gmssl/x509.h>
#include <gmssl/x509_ext.h>
#include <gmssl/oid.h>
#include <gmssl/pem.h>
#include <gmssl/mem.h>
#include <time.h>

#define TLS_TEST_PKI_VALID_SECONDS 31536000L

typedef struct tls_test_certificate {
  uint8_t der[4096];
  size_t size;
} tls_test_certificate;

static void tls_test_pki_free_key(X509_KEY *key) {
  if (key) { x509_key_cleanup(key); free(key); }
}

static X509_KEY *tls_test_pki_generate_key(void) {
  int curve = OID_secp256r1;
  X509_KEY *key = (X509_KEY *)calloc(1, sizeof(*key));
  if (key && x509_key_generate(key, OID_ec_public_key, &curve, sizeof(curve)) != 1) {
    tls_test_pki_free_key(key);
    key = NULL;
  }
  return key;
}

/* Generated identities use P-256.
 * Bounds cover the fixed key type, bounded CN, localhost SAN and extensions. */
static tls_test_certificate *tls_test_pki_make_certificate(
    X509_KEY *key, const tls_test_certificate *issuer, X509_KEY *issuer_key,
    unsigned char serial, const char *common_name, int is_ca, int localhost_san,
    int purpose, long not_before_offset, long not_after_offset) {
  tls_test_certificate *cert = NULL;
  uint8_t subject[256], extensions[512], names[64];
  size_t subject_size = 0, extensions_size = 0, names_size = 0, issuer_size;
  const uint8_t *issuer_name;
  const uint8_t loopback[4] = {127, 0, 0, 1};
  uint8_t *out;
  time_t now = time(NULL);
  if (!key || !common_name || !common_name[0] || strlen(common_name) > 128 ||
      now == (time_t)-1) return NULL;
  if (x509_name_set(subject, &subject_size, sizeof(subject), NULL, NULL, NULL,
                    NULL, NULL, common_name) != 1) return NULL;
  issuer_name = subject;
  issuer_size = subject_size;
  if (issuer && x509_cert_get_subject(issuer->der, issuer->size, &issuer_name, &issuer_size) != 1)
    return NULL;
  if (x509_exts_add_basic_constraints(extensions, &extensions_size, sizeof(extensions),
                                      1, is_ca, is_ca ? 1 : -1) != 1 ||
      x509_exts_add_key_usage(extensions, &extensions_size, sizeof(extensions), 1,
          is_ca ? X509_KU_KEY_CERT_SIGN | X509_KU_CRL_SIGN : X509_KU_DIGITAL_SIGNATURE) != 1)
    return NULL;
  if (!is_ca && purpose && x509_exts_add_ext_key_usage(extensions, &extensions_size,
                                        sizeof(extensions), 0, &purpose, 1) != 1) return NULL;
  if (localhost_san &&
      (x509_general_names_add_dns_name(names, &names_size, sizeof(names), "localhost") != 1 ||
       x509_general_names_add_general_name(names, &names_size, sizeof(names),
                                           X509_gn_ip_address, loopback, sizeof(loopback)) != 1 ||
       x509_exts_add_subject_alt_name(extensions, &extensions_size, sizeof(extensions),
                                      0, names, names_size) != 1)) return NULL;
  cert = (tls_test_certificate *)calloc(1, sizeof(*cert));
  if (!cert) return NULL;
  out = cert->der;
  if (x509_cert_sign_to_der(X509_version_v3, &serial, 1, OID_ecdsa_with_sha256,
         issuer_name, issuer_size, now + not_before_offset, now + not_after_offset,
         subject, subject_size, key, NULL, 0, NULL, 0, extensions, extensions_size,
         issuer_key ? issuer_key : key, NULL, 0, &out, &cert->size) != 1) {
    free(cert);
    return NULL;
  }
  return cert;
}

static tls_test_certificate *tls_test_pki_generate_self_signed(
    X509_KEY *key, const char *common_name, int is_ca) {
  return tls_test_pki_make_certificate(key, NULL, NULL, 1, common_name, is_ca, 0,
                                       OID_kp_client_auth, -300, TLS_TEST_PKI_VALID_SECONDS);
}

static int tls_test_pki_write_key(const char *path, X509_KEY *key) {
  uint8_t der[1024], *out = der;
  size_t size = 0;
  FILE *file;
  int rc;
  if (!path || !key) return -1;
  if (x509_private_key_info_to_der(key, &out, &size) != 1) {
    gmssl_secure_clear(der, sizeof(der));
    return -1;
  }
  file = fopen(path, "wb");
  rc = file && pem_write(file, "PRIVATE KEY", der, size) == 1 ? 0 : -1;
  if (file && fclose(file) != 0) rc = -1;
  gmssl_secure_clear(der, sizeof(der));
  return rc;
}

static int tls_test_pki_write_certificate(const char *path, const tls_test_certificate *cert) {
  FILE *file;
  int rc;
  if (!path || !cert) return -1;
  file = fopen(path, "wb");
  rc = file && x509_cert_to_pem(cert->der, cert->size, file) == 1 ? 0 : -1;
  if (file && fclose(file) != 0) rc = -1;
  return rc;
}

static int tls_test_write_self_signed_files(char *cert_path, size_t cert_path_len,
                                            char *key_path, size_t key_path_len,
                                            const char *common_name, int is_ca) {
  X509_KEY *key = NULL;
  tls_test_certificate *certificate = NULL;
  int rc = -1;
  if (!cert_path || cert_path_len == 0u || !key_path || key_path_len == 0u) return -1;
  cert_path[0] = '\0';
  key_path[0] = '\0';
  if (tls_test_write_temp_file(cert_path, cert_path_len, "crt", "") != 0 ||
      tls_test_write_temp_file(key_path, key_path_len, "key", "") != 0) goto done;
  key = tls_test_pki_generate_key();
  certificate = tls_test_pki_generate_self_signed(key, common_name, is_ca);
  if (!key || !certificate || tls_test_pki_write_certificate(cert_path, certificate) != 0 ||
      tls_test_pki_write_key(key_path, key) != 0) goto done;
  rc = 0;
done:
  free(certificate);
  tls_test_pki_free_key(key);
  if (rc != 0) {
    tls_test_remove_file(key_path);
    tls_test_remove_file(cert_path);
    key_path[0] = '\0';
    cert_path[0] = '\0';
  }
  return rc;
}

static int tls_test_write_unrelated_key_file(char *key_path, size_t key_path_len) {
  X509_KEY *key = NULL;
  int rc = -1;
  if (!key_path || key_path_len == 0u) return -1;
  key_path[0] = '\0';
  if (tls_test_write_temp_file(key_path, key_path_len, "key", "") != 0) return -1;
  key = tls_test_pki_generate_key();
  if (key && tls_test_pki_write_key(key_path, key) == 0) rc = 0;
  tls_test_pki_free_key(key);
  if (rc != 0) {
    tls_test_remove_file(key_path);
    key_path[0] = '\0';
  }
  return rc;
}

static int tls_test_write_expired_server_files(char *cert_path, size_t cert_path_len,
                                               char *key_path, size_t key_path_len) {
  X509_KEY *key = NULL;
  tls_test_certificate *certificate = NULL;
  int rc = -1;
  if (!cert_path || cert_path_len == 0u || !key_path || key_path_len == 0u) return -1;
  cert_path[0] = '\0';
  key_path[0] = '\0';
  if (tls_test_write_temp_file(cert_path, cert_path_len, "expired-crt", "") != 0 ||
      tls_test_write_temp_file(key_path, key_path_len, "expired-key", "") != 0) goto done;
  key = tls_test_pki_generate_key();
  certificate = tls_test_pki_make_certificate(key, NULL, NULL, 1, "localhost", 0,
                                               1, OID_kp_server_auth, -7200L, -3600L);
  if (!key || !certificate ||
      tls_test_pki_write_certificate(cert_path, certificate) != 0 ||
      tls_test_pki_write_key(key_path, key) != 0) goto done;
  rc = 0;
done:
  free(certificate);
  tls_test_pki_free_key(key);
  if (rc != 0) {
    tls_test_remove_file(key_path);
    tls_test_remove_file(cert_path);
    key_path[0] = '\0';
    cert_path[0] = '\0';
  }
  return rc;
}

#endif
