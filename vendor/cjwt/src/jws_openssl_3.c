/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/objects.h>
#include <limits.h>
#include <salts/crypto.h>
#include <stddef.h>
#include <stdint.h>
#include <platform.h>

#define _CRT_SECURE_NO_WARNINGS
#include "cjwt.h"
#include "jws.h"
#include "utils.h"

#define CJWT_CRYPTO_OK 0
#define CJWT_SHA256_SIZE 32U

static int cjwt_crypto_verify(const uint8_t *left, const uint8_t *right, size_t size)
{
    if ((!left || !right) && size != 0U) return -1;
    return CRYPTO_memcmp(left, right, size) == 0 ? CJWT_CRYPTO_OK : -1;
}

static void cjwt_crypto_wipe(void *data, size_t size)
{
    if (data && size != 0U) OPENSSL_cleanse(data, size);
}

static int cjwt_crypto_hmac_sha256(const uint8_t *key, size_t key_size,
                                   const uint8_t *message, size_t message_size,
                                   uint8_t output[CJWT_SHA256_SIZE])
{
    unsigned int output_size = 0U;
    if ((!key && key_size != 0U) || (!message && message_size != 0U) || !output ||
        key_size > INT_MAX)
        return -1;
    return HMAC(EVP_sha256(), key, (int)key_size, message, message_size, output, &output_size) &&
                   output_size == CJWT_SHA256_SIZE
               ? CJWT_CRYPTO_OK
               : -1;
}

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/
static cjwt_code_t process_okp_jwk(json_value_t *json, void **pkey,
                                    jws_pkey_type_t *pkey_type);

cjwt_code_t verify_hmac(const EVP_MD *sha, const struct sig_input *in)
{
    cjwt_code_t rv     = CJWTE_SIGNATURE_VALIDATION_FAILED;
    HMAC_CTX *hmac_ctx = NULL;
    unsigned int size = 0;
    uint8_t buff[EVP_MAX_MD_SIZE];

    if (INT_MAX < in->key.len) {
        return CJWTE_KEY_TOO_LARGE;
    }

    hmac_ctx = HMAC_CTX_new();
    if (hmac_ctx
        && (1 == HMAC_Init_ex(hmac_ctx, in->key.data, in->key.len, sha, NULL))
        && (1 == HMAC_Update(hmac_ctx, in->full.data, in->full.len))
        && (1 == HMAC_Final(hmac_ctx, buff, &size))
        && (in->sig.len == size)
        && (CJWT_CRYPTO_OK == cjwt_crypto_verify(in->sig.data, buff, size)))
    {
        rv = CJWTE_OK;
    }
    HMAC_CTX_free(hmac_ctx);
    cjwt_crypto_wipe(buff, sizeof(buff));
    return rv;
}

static cjwt_code_t verify_hmac_sha256(const struct sig_input *in)
{
    uint8_t digest[CJWT_SHA256_SIZE];
    cjwt_code_t rv = CJWTE_SIGNATURE_VALIDATION_FAILED;

    if (!in || (!in->key.data && in->key.len != 0U) ||
        (!in->full.data && in->full.len != 0U)) {
        return CJWTE_INVALID_PARAMETERS;
    }
    if (cjwt_crypto_hmac_sha256(in->key.data, in->key.len,
                               in->full.data, in->full.len,
                               digest) == CJWT_CRYPTO_OK &&
        in->sig.len == sizeof(digest) &&
        cjwt_crypto_verify(in->sig.data, digest, sizeof(digest)) ==
            CJWT_CRYPTO_OK) {
        rv = CJWTE_OK;
    }
    cjwt_crypto_wipe(digest, sizeof(digest));
    return rv;
}

int add_padding(int type, EVP_PKEY_CTX *ctx, int padding)
{
    if (EVP_PKEY_EC == type) {
        return 1;
    }

    return EVP_PKEY_CTX_set_rsa_padding(ctx, padding);
}

int calc_sig(int type, const struct sig_input *in, uint8_t **sig, int *len)
{
    int rv               = 0; /* Match the other openssl symantics for consistency */
    ECDSA_SIG *ecdsa_sig = NULL;
    BIGNUM *pr           = NULL;
    BIGNUM *ps           = NULL;
    int new_sig_len      = 0;
    uint8_t *new_sig     = NULL;

    if (EVP_PKEY_RSA == type) {
        *sig = (uint8_t *) in->sig.data;
        *len = (int)in->sig.len;
        return 1;
    }

    ecdsa_sig = ECDSA_SIG_new();
    if (ecdsa_sig == NULL) {
        return 0;
    }

    /* Read out the r,s numbers from the signature for later.
     * We must convert from this format into DEC because that's
     * all openssl supports. */
    pr = BN_bin2bn(in->sig.data, (int) in->sig.len / 2, NULL);
    ps = BN_bin2bn(in->sig.data + in->sig.len / 2, (int) in->sig.len / 2, NULL);

    if (1 == ECDSA_SIG_set0(ecdsa_sig, pr, ps)) {
        new_sig_len = i2d_ECDSA_SIG(ecdsa_sig, &new_sig);
        if (0 <= new_sig_len) {
            /* We don't own the memory now, don't free it. */
            pr = NULL;
            ps = NULL;

            if (0 < new_sig_len) {
                *sig    = new_sig;
                *len    = new_sig_len;
                new_sig = NULL; /* Passed back now, so don't free the buffer. */
                rv      = 1;
            }
        }
    }

    OPENSSL_free(new_sig);
    ECDSA_SIG_free(ecdsa_sig);
    BN_free(ps);
    BN_free(pr);

    return rv;
}

cjwt_code_t verify_most(const EVP_MD *sha, const struct sig_input *in, int type, int padding)
{
    cjwt_code_t rv         = CJWTE_SIGNATURE_VALIDATION_FAILED;
    EVP_MD_CTX *md_ctx     = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;
    EVP_PKEY *pkey         = NULL;
    BIO *keybio            = NULL;
    int sig_len            = 0;
    uint8_t *sig           = NULL;

    if (!in) return CJWTE_INVALID_PARAMETERS;

    if (in->pkey) {
        pkey = (EVP_PKEY *)in->pkey;
        EVP_PKEY_up_ref(pkey);
    } else {
        if ((0 == in->key.len) || (NULL == in->key.data)) {
            return CJWTE_SIGNATURE_MISSING_KEY;
        }

        /* Read the RSA key in from a PEM encoded blob of memory */
        keybio = BIO_new_mem_buf(in->key.data, (int) in->key.len);
        if (!keybio) {
            return CJWTE_OUT_OF_MEMORY;
        }

        pkey = PEM_read_bio_PUBKEY(keybio, NULL, NULL, NULL);
        if (!pkey) {
            rv = CJWTE_SIGNATURE_INVALID_KEY;
            goto done;
        }
    }

    if (type != EVP_PKEY_id(pkey)) {
        rv = CJWTE_SIGNATURE_INVALID_KEY;
        goto done;
    }

    md_ctx = EVP_MD_CTX_create();

    if (type == EVP_PKEY_ED25519 || type == EVP_PKEY_ED448) {
        sig = (uint8_t *)in->sig.data;
        sig_len = (int)in->sig.len;
        if (md_ctx
            && (1 == EVP_DigestVerifyInit(md_ctx, &pkey_ctx, NULL, NULL, pkey))
            && (1 == EVP_DigestVerify(md_ctx, sig, sig_len,
                                       in->full.data, in->full.len))) {
            rv = CJWTE_OK;
        }
        goto done;
    }

    if (md_ctx
        && (1 == calc_sig(type, in, &sig, &sig_len))
        && (1 == EVP_DigestVerifyInit(md_ctx, &pkey_ctx, sha, NULL, pkey))
        && (type != EVP_PKEY_ED25519 && type != EVP_PKEY_ED448 ? 0 < add_padding(type, pkey_ctx, padding) : 1)
        && (1 == EVP_DigestVerifyUpdate(md_ctx, in->full.data, in->full.len))
        && (1 == EVP_DigestVerifyFinal(md_ctx, sig, sig_len)))
    {
        rv = CJWTE_OK;
    }

done:

    if (sig != in->sig.data) OPENSSL_free(sig);

    if (keybio) BIO_free(keybio);
    if (pkey) EVP_PKEY_free(pkey);
    if (md_ctx) EVP_MD_CTX_free(md_ctx);

    return rv;
}

static cjwt_code_t verify_ed448(const struct sig_input *in)
{
    const uint8_t *public_key;

    if (!in || in->full.len > UINT32_MAX) return CJWTE_INVALID_PARAMETERS;
    if (in->sig.len != SALTS_CRYPTO_ED448_SIGNATURE_SIZE) {
        return CJWTE_SIGNATURE_VALIDATION_FAILED;
    }

    if (in->pkey) {
        if (in->pkey_type != JWS_PKEY_ED448_PUBLIC) {
            return CJWTE_SIGNATURE_INVALID_KEY;
        }
        public_key = (const uint8_t *)in->pkey;
    } else {
        if (!in->key.data
            || in->key.len != SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE) {
            return CJWTE_SIGNATURE_INVALID_KEY;
        }
        public_key = in->key.data;
    }

    return salts_crypto_ed448_verify(public_key, in->full.data, in->full.len,
                                     in->sig.data) == SALTS_CRYPTO_OK
               ? CJWTE_OK
               : CJWTE_SIGNATURE_VALIDATION_FAILED;
}


/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/
cjwt_code_t jws_verify_signature(const cjwt_t *jwt, const struct sig_input *in)
{
    switch (jwt->header.alg) {
        case alg_es256:
            return verify_most(EVP_sha256(), in, EVP_PKEY_EC, 0);
        case alg_es384:
            return verify_most(EVP_sha384(), in, EVP_PKEY_EC, 0);
        case alg_es512:
            return verify_most(EVP_sha512(), in, EVP_PKEY_EC, 0);

        case alg_hs256:
            return verify_hmac_sha256(in);
        case alg_hs384:
            return verify_hmac(EVP_sha384(), in);
        case alg_hs512:
            return verify_hmac(EVP_sha512(), in);

        case alg_ps256:
            return verify_most(EVP_sha256(), in, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING);
        case alg_ps384:
            return verify_most(EVP_sha384(), in, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING);
        case alg_ps512:
            return verify_most(EVP_sha512(), in, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING);

        case alg_rs256:
            return verify_most(EVP_sha256(), in, EVP_PKEY_RSA, RSA_PKCS1_PADDING);
        case alg_rs384:
            return verify_most(EVP_sha384(), in, EVP_PKEY_RSA, RSA_PKCS1_PADDING);
        case alg_rs512:
            return verify_most(EVP_sha512(), in, EVP_PKEY_RSA, RSA_PKCS1_PADDING);

        case alg_es256k:
            return verify_most(EVP_sha256(), in, EVP_PKEY_EC, 0);

        case alg_eddsa:
            if (in->pkey_type == JWS_PKEY_ED448_PUBLIC
                || (!in->pkey
                    && in->key.len == SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE)) {
                return verify_ed448(in);
            }
            if (in->pkey && in->pkey_type == JWS_PKEY_EVP
                && EVP_PKEY_id((EVP_PKEY *)in->pkey) == EVP_PKEY_ED448) {
                return verify_most(NULL, in, EVP_PKEY_ED448, 0);
            }
            return verify_most(NULL, in, EVP_PKEY_ED25519, 0);

        default:
            break;
    }

    return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
}

static cjwt_code_t sign_hmac(const EVP_MD *sha, const uint8_t *full, size_t full_len,
                             const uint8_t *key, size_t key_len,
                             uint8_t **sig, size_t *sig_len)
{
    cjwt_code_t rv     = CJWTE_OUT_OF_MEMORY;
    HMAC_CTX *hmac_ctx = NULL;
    unsigned int size = 0;
    uint8_t buff[EVP_MAX_MD_SIZE];

    hmac_ctx = HMAC_CTX_new();
    if (hmac_ctx
        && (1 == HMAC_Init_ex(hmac_ctx, key, key_len, sha, NULL))
        && (1 == HMAC_Update(hmac_ctx, full, full_len))
        && (1 == HMAC_Final(hmac_ctx, buff, &size)))
    {
        *sig = malloc(size);
        if (*sig) {
            memcpy(*sig, buff, size);
            *sig_len = size;
            rv = CJWTE_OK;
        }
    }
    HMAC_CTX_free(hmac_ctx);
    cjwt_crypto_wipe(buff, sizeof(buff));
    return rv;
}

static cjwt_code_t sign_hmac_sha256(const uint8_t *full, size_t full_len,
                                    const uint8_t *key, size_t key_len,
                                    uint8_t **sig, size_t *sig_len)
{
    uint8_t digest[CJWT_SHA256_SIZE];
    cjwt_code_t rv = CJWTE_OUT_OF_MEMORY;

    if (!sig || !sig_len || (!full && full_len != 0U) ||
        (!key && key_len != 0U)) {
        return CJWTE_INVALID_PARAMETERS;
    }
    if (cjwt_crypto_hmac_sha256(key, key_len, full, full_len, digest) !=
        CJWT_CRYPTO_OK) {
        return CJWTE_SIGNATURE_VALIDATION_FAILED;
    }

    *sig = malloc(sizeof(digest));
    if (*sig) {
        memcpy(*sig, digest, sizeof(digest));
        *sig_len = sizeof(digest);
        rv = CJWTE_OK;
    }
    cjwt_crypto_wipe(digest, sizeof(digest));
    return rv;
}

static cjwt_code_t sign_most(const EVP_MD *sha, const uint8_t *full, size_t full_len,
                             const uint8_t *key_data, size_t key_len,
                             int type, int padding,
                             uint8_t **sig, size_t *sig_len)
{
    cjwt_code_t rv         = CJWTE_OUT_OF_MEMORY;
    EVP_MD_CTX *md_ctx     = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;
    EVP_PKEY *pkey         = NULL;
    BIO *keybio            = NULL;
    uint8_t *tmp_sig       = NULL;
    size_t tmp_sig_len     = 0;

    keybio = BIO_new_mem_buf(key_data, (int) key_len);
    if (!keybio) return CJWTE_OUT_OF_MEMORY;

    pkey = PEM_read_bio_PrivateKey(keybio, NULL, NULL, NULL);
    if (!pkey) {
        BIO_free(keybio);
        return CJWTE_SIGNATURE_INVALID_KEY;
    }

    md_ctx = EVP_MD_CTX_create();

    if (EVP_PKEY_id(pkey) == EVP_PKEY_ED25519
        || EVP_PKEY_id(pkey) == EVP_PKEY_ED448) {
        if (md_ctx
            && (1 == EVP_DigestSignInit(md_ctx, &pkey_ctx, NULL, NULL, pkey))
            && (1 == EVP_DigestSign(md_ctx, NULL, &tmp_sig_len,
                                    full, full_len))) {
            tmp_sig = malloc(tmp_sig_len);
            if (tmp_sig
                && (1 == EVP_DigestSign(md_ctx, tmp_sig, &tmp_sig_len,
                                         full, full_len))) {
                *sig = tmp_sig;
                *sig_len = tmp_sig_len;
                tmp_sig = NULL;
                rv = CJWTE_OK;
            }
        }
        goto sign_cleanup;
    }

    if (md_ctx
        && (1 == EVP_DigestSignInit(md_ctx, &pkey_ctx, sha, NULL, pkey))
        && (type != EVP_PKEY_ED25519 && type != EVP_PKEY_ED448 ? 0 < add_padding(type, pkey_ctx, padding) : 1)
        && (1 == EVP_DigestSignUpdate(md_ctx, full, full_len))
        && (1 == EVP_DigestSignFinal(md_ctx, NULL, &tmp_sig_len)))
    {
        tmp_sig = malloc(tmp_sig_len);
        if (tmp_sig && (1 == EVP_DigestSignFinal(md_ctx, tmp_sig, &tmp_sig_len))) {
            if (type == EVP_PKEY_EC) {
                /* EC signature is DER (Sequence of r, s). Need to convert to raw r|s */
                const uint8_t *der_cursor = tmp_sig;
                ECDSA_SIG *ec_sig = d2i_ECDSA_SIG(NULL, &der_cursor, (long)tmp_sig_len);
                if (ec_sig) {
                    const BIGNUM *r, *s;
                    ECDSA_SIG_get0(ec_sig, &r, &s);
                    int degree = EVP_PKEY_bits(pkey);
                    int order_len = (degree + 7) / 8;
                    *sig_len = 2 * order_len;
                    *sig = calloc(1, *sig_len);
                    if (*sig) {
                        BN_bn2binpad(r, *sig, order_len);
                        BN_bn2binpad(s, *sig + order_len, order_len);
                        rv = CJWTE_OK;
                    }
                    ECDSA_SIG_free(ec_sig);
                }
            } else {
                *sig = tmp_sig;
                *sig_len = tmp_sig_len;
                tmp_sig = NULL;
                rv = CJWTE_OK;
            }
        }
    }

sign_cleanup:
    free(tmp_sig);
    BIO_free(keybio);
    EVP_PKEY_free(pkey);
    EVP_MD_CTX_free(md_ctx);
    return rv;
}

static cjwt_code_t sign_ed448(const uint8_t *full, size_t full_len,
                              const uint8_t *key, size_t key_len,
                              uint8_t **sig, size_t *sig_len)
{
    uint8_t *output;

    if (!sig || !sig_len || !key
        || key_len != SALTS_CRYPTO_ED448_PRIVATE_KEY_SIZE
        || full_len > UINT32_MAX) {
        return CJWTE_INVALID_PARAMETERS;
    }

    output = malloc(SALTS_CRYPTO_ED448_SIGNATURE_SIZE);
    if (!output) return CJWTE_OUT_OF_MEMORY;
    if (salts_crypto_ed448_sign(key, full, full_len, output) != SALTS_CRYPTO_OK) {
        cjwt_crypto_wipe(output, SALTS_CRYPTO_ED448_SIGNATURE_SIZE);
        free(output);
        return CJWTE_SIGNATURE_INVALID_KEY;
    }

    *sig = output;
    *sig_len = SALTS_CRYPTO_ED448_SIGNATURE_SIZE;
    return CJWTE_OK;
}

cjwt_code_t jws_sign(const cjwt_alg_t alg, const uint8_t *full, size_t full_len,
                     const uint8_t *key, size_t key_len,
                     uint8_t **sig, size_t *sig_len)
{
    switch (alg) {
        case alg_es256:
            return sign_most(EVP_sha256(), full, full_len, key, key_len, EVP_PKEY_EC, 0, sig, sig_len);
        case alg_es384:
            return sign_most(EVP_sha384(), full, full_len, key, key_len, EVP_PKEY_EC, 0, sig, sig_len);
        case alg_es512:
            return sign_most(EVP_sha512(), full, full_len, key, key_len, EVP_PKEY_EC, 0, sig, sig_len);

        case alg_hs256:
            return sign_hmac_sha256(full, full_len, key, key_len, sig, sig_len);
        case alg_hs384:
            return sign_hmac(EVP_sha384(), full, full_len, key, key_len, sig, sig_len);
        case alg_hs512:
            return sign_hmac(EVP_sha512(), full, full_len, key, key_len, sig, sig_len);

        case alg_ps256:
            return sign_most(EVP_sha256(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, sig, sig_len);
        case alg_ps384:
            return sign_most(EVP_sha384(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, sig, sig_len);
        case alg_ps512:
            return sign_most(EVP_sha512(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, sig, sig_len);

        case alg_rs256:
            return sign_most(EVP_sha256(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PADDING, sig, sig_len);
        case alg_rs384:
            return sign_most(EVP_sha384(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PADDING, sig, sig_len);
        case alg_rs512:
            return sign_most(EVP_sha512(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PADDING, sig, sig_len);

        case alg_es256k:
            return sign_most(EVP_sha256(), full, full_len, key, key_len, EVP_PKEY_EC, 0, sig, sig_len);

        case alg_eddsa:
            if (key_len == SALTS_CRYPTO_ED448_PRIVATE_KEY_SIZE) {
                return sign_ed448(full, full_len, key, key_len, sig, sig_len);
            }
            return sign_most(NULL, full, full_len, key, key_len, EVP_PKEY_ED25519, 0, sig, sig_len);

        default:
            break;
    }

    return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
}

static uint8_t *json_to_bytes(json_value_t *json, const char *name, size_t *len)
{
    const char *text = json_get_string(json, name);
    if (!text) return NULL;
    return b64url_decode_with_alloc((const uint8_t *)text, strlen(text), len);
}

static BIGNUM *json_to_bn(json_value_t *json, const char *name)
{
    size_t len = 0;
    uint8_t *bytes = json_to_bytes(json, name, &len);
    BIGNUM *bn = NULL;

    if (bytes && len <= INT_MAX) bn = BN_bin2bn(bytes, (int)len, NULL);
    free(bytes);
    return bn;
}

static cjwt_code_t process_rsa_jwk(json_value_t *json, EVP_PKEY **pkey)
{
    BIGNUM *n = json_to_bn(json, "n");
    BIGNUM *e = json_to_bn(json, "e");
    BIGNUM *d = json_to_bn(json, "d");
    BIGNUM *p = json_to_bn(json, "p");
    BIGNUM *q = json_to_bn(json, "q");
    BIGNUM *dp = json_to_bn(json, "dp");
    BIGNUM *dq = json_to_bn(json, "dq");
    BIGNUM *qi = json_to_bn(json, "qi");
    RSA *rsa = NULL;
    EVP_PKEY *out = NULL;
    cjwt_code_t rv = CJWTE_INVALID_PARAMETERS;

    if (!n || !e) goto cleanup;

    if (!d) {
        rsa = RSA_new_public_key(n, e);
    } else if (p && q && dp && dq && qi) {
        rsa = RSA_new_private_key(n, e, d, p, q, dp, dq, qi);
    } else {
        rsa = RSA_new_private_key_no_crt(n, e, d);
    }
    if (!rsa) goto cleanup;

    out = EVP_PKEY_new();
    if (!out) {
        rv = CJWTE_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (EVP_PKEY_assign_RSA(out, rsa) != 1) goto cleanup;
    rsa = NULL;
    *pkey = out;
    out = NULL;
    rv = CJWTE_OK;

cleanup:
    EVP_PKEY_free(out);
    RSA_free(rsa);
    BN_free(qi);
    BN_free(dq);
    BN_free(dp);
    BN_free(q);
    BN_free(p);
    BN_free(d);
    BN_free(e);
    BN_free(n);
    return rv;
}

static int jwk_curve_nid(const char *name)
{
    if (!strcmp(name, "P-256")) return NID_X9_62_prime256v1;
    if (!strcmp(name, "P-384")) return NID_secp384r1;
    if (!strcmp(name, "P-521")) return NID_secp521r1;
    if (!strcmp(name, "secp256k1") || !strcmp(name, "K-256")) {
        return NID_secp256k1;
    }
    return NID_undef;
}

static cjwt_code_t process_ec_jwk(json_value_t *json, EVP_PKEY **pkey)
{
    const char *curve = json_get_string(json, "crv");
    BIGNUM *x = json_to_bn(json, "x");
    BIGNUM *y = json_to_bn(json, "y");
    BIGNUM *d = json_to_bn(json, "d");
    EC_KEY *ec = NULL;
    EVP_PKEY *out = NULL;
    cjwt_code_t rv = CJWTE_INVALID_PARAMETERS;
    int nid = curve ? jwk_curve_nid(curve) : NID_undef;

    if (nid == NID_undef || !x || !y) goto cleanup;
    ec = EC_KEY_new_by_curve_name(nid);
    if (!ec) goto cleanup;
    if (EC_KEY_set_public_key_affine_coordinates(ec, x, y) != 1) goto cleanup;
    if (d && EC_KEY_set_private_key(ec, d) != 1) goto cleanup;
    if (EC_KEY_check_key(ec) != 1) goto cleanup;

    out = EVP_PKEY_new();
    if (!out) {
        rv = CJWTE_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (EVP_PKEY_assign_EC_KEY(out, ec) != 1) goto cleanup;
    ec = NULL;
    *pkey = out;
    out = NULL;
    rv = CJWTE_OK;

cleanup:
    EVP_PKEY_free(out);
    EC_KEY_free(ec);
    BN_free(d);
    BN_free(y);
    BN_free(x);
    return rv;
}

static cjwt_code_t process_ed448_jwk(json_value_t *json, void **pkey,
                                     jws_pkey_type_t *pkey_type)
{
    json_value_t *public_json = json_object_get(json, "x");
    json_value_t *private_json = json_object_get(json, "d");
    size_t public_len = 0;
    size_t private_len = 0;
    uint8_t *public_key = json_to_bytes(json, "x", &public_len);
    uint8_t *private_key = json_to_bytes(json, "d", &private_len);
    uint8_t derived[SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE] = {0};
    const uint8_t *key_to_copy = public_key;
    uint8_t *out = NULL;
    cjwt_code_t rv = CJWTE_INVALID_PARAMETERS;

    if ((!public_json && !private_json)
        || (public_json
            && (!public_key
                || public_len != SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE))
        || (private_json
            && (!private_key
                || private_len != SALTS_CRYPTO_ED448_PRIVATE_KEY_SIZE))) {
        goto cleanup;
    }

    if (private_key) {
        if (salts_crypto_ed448_public_key(private_key, derived)
            != SALTS_CRYPTO_OK) {
            goto cleanup;
        }
        if (public_key
            && cjwt_crypto_verify(derived, public_key, sizeof(derived))
                   != CJWT_CRYPTO_OK) {
            goto cleanup;
        }
        key_to_copy = derived;
    }

    out = malloc(SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE);
    if (!out) {
        rv = CJWTE_OUT_OF_MEMORY;
        goto cleanup;
    }
    memcpy(out, key_to_copy, SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE);
    *pkey = out;
    *pkey_type = JWS_PKEY_ED448_PUBLIC;
    out = NULL;
    rv = CJWTE_OK;

cleanup:
    free(out);
    cjwt_crypto_wipe(derived, sizeof(derived));
    if (private_key) cjwt_crypto_wipe(private_key, private_len);
    free(private_key);
    free(public_key);
    return rv;
}

static cjwt_code_t process_okp_jwk(json_value_t *json, void **pkey,
                                    jws_pkey_type_t *pkey_type)
{
    const char *curve = json_get_string(json, "crv");
    if (curve && strcmp(curve, "Ed448") == 0) {
        return process_ed448_jwk(json, pkey, pkey_type);
    }

    size_t public_len = 0;
    size_t private_len = 0;
    uint8_t *public_key = json_to_bytes(json, "x", &public_len);
    uint8_t *private_key = json_to_bytes(json, "d", &private_len);
    EVP_PKEY *out = NULL;
    cjwt_code_t rv = CJWTE_INVALID_PARAMETERS;

    if (!curve || strcmp(curve, "Ed25519") != 0) {
        rv = CJWTE_SIGNATURE_UNSUPPORTED_ALG;
        goto cleanup;
    }
    if (private_key) {
        if (private_len != 32U) goto cleanup;
        out = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key,
                                           private_len);
    } else if (public_key) {
        if (public_len != 32U) goto cleanup;
        out = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key,
                                          public_len);
    }
    if (!out) goto cleanup;

    if (private_key && public_key) {
        uint8_t derived[32];
        size_t derived_len = sizeof(derived);
        if (public_len != sizeof(derived)
            || EVP_PKEY_get_raw_public_key(out, derived, &derived_len) != 1
            || derived_len != public_len
            || cjwt_crypto_verify(derived, public_key, public_len) != CJWT_CRYPTO_OK)
        {
            cjwt_crypto_wipe(derived, sizeof(derived));
            goto cleanup;
        }
        cjwt_crypto_wipe(derived, sizeof(derived));
    }

    *pkey = out;
    *pkey_type = JWS_PKEY_EVP;
    out = NULL;
    rv = CJWTE_OK;

cleanup:
    EVP_PKEY_free(out);
    cjwt_crypto_wipe(private_key, private_len);
    free(private_key);
    free(public_key);
    return rv;
}

cjwt_code_t jws_jwk_to_pkey(const cjwt_jwk_t *jwk, void **pkey,
                             jws_pkey_type_t *pkey_type)
{
    if (!jwk || !jwk->key_json || !pkey || !pkey_type) {
        return CJWTE_INVALID_PARAMETERS;
    }
    *pkey = NULL;
    *pkey_type = JWS_PKEY_EVP;
    if (jwk->kty == CJWT_KTY_RSA) return process_rsa_jwk(jwk->key_json, (EVP_PKEY **)pkey);
    if (jwk->kty == CJWT_KTY_EC) return process_ec_jwk(jwk->key_json, (EVP_PKEY **)pkey);
    if (jwk->kty == CJWT_KTY_OKP) return process_okp_jwk(jwk->key_json, pkey, pkey_type);
    return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
}

void jws_pkey_free(void *pkey, jws_pkey_type_t pkey_type)
{
    if (!pkey) return;
    if (pkey_type == JWS_PKEY_ED448_PUBLIC) {
        free(pkey);
    } else {
        EVP_PKEY_free((EVP_PKEY *)pkey);
    }
}
