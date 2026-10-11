/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __JWS_H__
#define __JWS_H__

#include <stddef.h>
#include <stdint.h>

#include "cjwt.h"
#include <salts/crypto.h>

struct sig_section {
    const uint8_t *data;
    size_t len;
};

typedef enum {
    JWS_PKEY_SALTS = 0
} jws_pkey_type_t;

struct sig_input {
    struct sig_section full;
    struct sig_section sig;
    struct sig_section key;
    void *pkey; /* Optional borrowed immutable salts_crypto_key. */
    jws_pkey_type_t pkey_type;
};

const char *alg_to_string(cjwt_alg_t alg);
const char *enc_to_string(cjwt_enc_t enc);

cjwt_code_t jws_verify_signature(const cjwt_t *jwt, const struct sig_input *in);
cjwt_code_t jws_jwk_to_pkey(const cjwt_jwk_t *jwk, void **pkey,
                            jws_pkey_type_t *pkey_type);
void jws_pkey_free(void *pkey, jws_pkey_type_t pkey_type);
cjwt_code_t jws_load_key(const uint8_t *data, size_t size, const cjwt_jwk_t *jwk,
                         int private_key, salts_crypto_key **output);
cjwt_code_t jws_sign(const cjwt_alg_t alg, const uint8_t *full, size_t full_len, 
                     const uint8_t *key, size_t key_len, 
                     uint8_t **sig, size_t *sig_len);

#endif
