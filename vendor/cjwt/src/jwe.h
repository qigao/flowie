/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __JWE_H__
#define __JWE_H__

#include <stddef.h>
#include <stdint.h>

#include "cjwt.h"
#include "utils.h"

cjwt_code_t jwe_decrypt(const cjwt_t *jwt,
                        const struct section *header,
                        const struct section *enc_key,
                        const struct section *iv,
                        const struct section *ciphertext,
                        const struct section *tag,
                        const uint8_t *key_data, size_t key_len,
                        const cjwt_jwk_t *jwk,
                        uint8_t **plaintext, size_t *plaintext_len);

cjwt_code_t jwe_pbes2_prepare(cjwt_t *jwt);

cjwt_code_t jwe_encrypt(const cjwt_t *jwt,
                        const char *header_b64,
                        const uint8_t *plaintext, size_t plaintext_len,
                        const uint8_t *key_data, size_t key_len,
                        const cjwt_jwk_t *jwk,
                        char **output);

#endif

