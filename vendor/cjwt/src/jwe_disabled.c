/* SPDX-FileCopyrightText: 2026 Flowie contributors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "jwe.h"

cjwt_code_t jwe_decrypt(const cjwt_t *jwt, const struct section *header,
                        const struct section *enc_key, const struct section *iv,
                        const struct section *ciphertext, const struct section *tag,
                        const uint8_t *key_data, size_t key_len, const cjwt_jwk_t *jwk,
                        uint8_t **plaintext, size_t *plaintext_len) {
  (void)jwt;
  (void)header;
  (void)enc_key;
  (void)iv;
  (void)ciphertext;
  (void)tag;
  (void)key_data;
  (void)key_len;
  (void)jwk;
  if (plaintext) *plaintext = NULL;
  if (plaintext_len) *plaintext_len = 0u;
  return CJWTE_HEADER_UNSUPPORTED_ALG;
}

cjwt_code_t jwe_pbes2_prepare(cjwt_t *jwt) {
  (void)jwt;
  return CJWTE_HEADER_UNSUPPORTED_ALG;
}

cjwt_code_t jwe_encrypt(const cjwt_t *jwt, const char *header_b64,
                        const uint8_t *plaintext, size_t plaintext_len,
                        const uint8_t *key_data, size_t key_len, const cjwt_jwk_t *jwk,
                        char **output) {
  (void)jwt;
  (void)header_b64;
  (void)plaintext;
  (void)plaintext_len;
  (void)key_data;
  (void)key_len;
  (void)jwk;
  if (output) *output = NULL;
  return CJWTE_HEADER_UNSUPPORTED_ALG;
}
