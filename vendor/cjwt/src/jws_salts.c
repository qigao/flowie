/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <salts/crypto.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "cjwt.h"
#include "jws.h"
#include "utils.h"

static int algorithm(cjwt_alg_t alg, salts_crypto_hash *hash, salts_crypto_signature *scheme,
                     salts_crypto_key_kind *kind) {
    *scheme = 0; *kind = 0;
    switch (alg) {
    case alg_hs256: *hash = SALTS_CRYPTO_SHA256; return 1;
    case alg_hs384: *hash = SALTS_CRYPTO_SHA384; return 1;
    case alg_hs512: *hash = SALTS_CRYPTO_SHA512; return 1;
    case alg_rs256: case alg_ps256: *hash = SALTS_CRYPTO_SHA256; break;
    case alg_rs384: case alg_ps384: *hash = SALTS_CRYPTO_SHA384; break;
    case alg_rs512: case alg_ps512: *hash = SALTS_CRYPTO_SHA512; break;
    case alg_es256: case alg_es256k:
        *hash = SALTS_CRYPTO_SHA256; *scheme = SALTS_CRYPTO_ECDSA; *kind = SALTS_CRYPTO_KEY_EC; return 1;
    case alg_es384:
        *hash = SALTS_CRYPTO_SHA384; *scheme = SALTS_CRYPTO_ECDSA; *kind = SALTS_CRYPTO_KEY_EC; return 1;
    case alg_es512:
        *hash = SALTS_CRYPTO_SHA512; *scheme = SALTS_CRYPTO_ECDSA; *kind = SALTS_CRYPTO_KEY_EC; return 1;
    case alg_eddsa:
        *hash = SALTS_CRYPTO_SHA512; *scheme = SALTS_CRYPTO_EDDSA; return 1;
    default: return 0;
    }
    *kind = SALTS_CRYPTO_KEY_RSA;
    *scheme = alg == alg_ps256 || alg == alg_ps384 || alg == alg_ps512 ? SALTS_CRYPTO_RSA_PSS : SALTS_CRYPTO_RSA_PKCS1;
    return 1;
}

static int key_matches(const salts_crypto_key *key, salts_crypto_key_kind kind) {
    salts_crypto_key_kind actual = salts_crypto_key_type(key);
    return kind ? actual == kind : actual == SALTS_CRYPTO_KEY_ED25519 || actual == SALTS_CRYPTO_KEY_ED448;
}

static int decode_field(json_value_t *json, const char *name, salts_crypto_bytes *value) {
    const char *text;
    if (!json_object_get(json, name)) return 0;
    text = json_get_string(json, name);
    if (!text) return -1;
    value->data = b64url_decode_with_alloc((const uint8_t *)text, strlen(text), &value->size);
    return value->data && value->size ? 1 : -1;
}

static salts_crypto_ec_curve curve_name(const char *name) {
    if (!name) return 0;
    if (!strcmp(name, "P-256")) return SALTS_CRYPTO_EC_P256;
    if (!strcmp(name, "P-384")) return SALTS_CRYPTO_EC_P384;
    if (!strcmp(name, "P-521")) return SALTS_CRYPTO_EC_P521;
    if (!strcmp(name, "secp256k1") || !strcmp(name, "K-256")) return SALTS_CRYPTO_EC_SECP256K1;
    return 0;
}

static int coordinate(uint8_t *out, size_t size, salts_crypto_bytes value) {
    while (value.size > 1 && !*value.data) { ++value.data; --value.size; }
    if (!value.size || value.size > size) return 0;
    memset(out, 0, size); memcpy(out + size - value.size, value.data, value.size);
    return 1;
}

cjwt_code_t jws_jwk_to_pkey(const cjwt_jwk_t *jwk, void **pkey, jws_pkey_type_t *pkey_type) {
    static const char *rsa_names[] = {"n", "e", "d", "p", "q", "dp", "dq", "qi"};
    salts_crypto_bytes values[8] = {{0}};
    salts_crypto_key *key = NULL;
    int status = SALTS_CRYPTO_EINVAL;
    size_t i;
    cjwt_code_t result = CJWTE_INVALID_PARAMETERS;
    if (!jwk || !jwk->key_json || !pkey || !pkey_type) return result;
    *pkey = NULL; *pkey_type = JWS_PKEY_SALTS;
    if (jwk->kty == CJWT_KTY_RSA) {
        for (i = 0; i < 8; ++i) if (decode_field(jwk->key_json, rsa_names[i], &values[i]) < 0) goto done;
        status = salts_crypto_key_from_rsa(values[0], values[1], values[2], values[3], values[4],
                                           values[5], values[6], values[7], &key);
    } else if (jwk->kty == CJWT_KTY_EC) {
        uint8_t point[133];
        salts_crypto_ec_curve curve = curve_name(json_get_string(jwk->key_json, "crv"));
        size_t n = curve == SALTS_CRYPTO_EC_P384 ? 48 : curve == SALTS_CRYPTO_EC_P521 ? 66 : 32;
        salts_crypto_bytes public_key = {point, 1 + 2*n};
        if (!curve || decode_field(jwk->key_json, "x", &values[0]) != 1 ||
            decode_field(jwk->key_json, "y", &values[1]) != 1 ||
            decode_field(jwk->key_json, "d", &values[2]) < 0) goto done;
        point[0] = 4;
        if (!coordinate(point + 1, n, values[0]) || !coordinate(point + 1 + n, n, values[1])) goto done;
        status = salts_crypto_key_from_ec(curve, public_key, values[2], &key);
    } else if (jwk->kty == CJWT_KTY_OKP) {
        const char *curve = json_get_string(jwk->key_json, "crv");
        salts_crypto_key_kind kind;
        if (!curve || (strcmp(curve, "Ed25519") && strcmp(curve, "Ed448"))) {
            result = CJWTE_SIGNATURE_UNSUPPORTED_ALG; goto done;
        }
        kind = !strcmp(curve, "Ed25519") ? SALTS_CRYPTO_KEY_ED25519 : SALTS_CRYPTO_KEY_ED448;
        if (decode_field(jwk->key_json, "x", &values[0]) < 0 || decode_field(jwk->key_json, "d", &values[1]) < 0)
            goto done;
        status = salts_crypto_key_from_ed(kind, values[0], values[1], &key);
    } else { result = CJWTE_SIGNATURE_UNSUPPORTED_ALG; goto done; }
    if (status == SALTS_CRYPTO_OK) { *pkey = key; key = NULL; result = CJWTE_OK; }
    else if (status == SALTS_CRYPTO_ENOMEM) result = CJWTE_OUT_OF_MEMORY;
done:
    for (i = 0; i < 8; ++i) {
        salts_crypto_clear((void *)values[i].data, values[i].size);
        free((void *)values[i].data);
    }
    salts_crypto_key_destroy(key);
    return result;
}

void jws_pkey_free(void *key, jws_pkey_type_t type) {
    (void)type;
    salts_crypto_key_destroy(key);
}

cjwt_code_t jws_load_key(const uint8_t *data, size_t size, const cjwt_jwk_t *jwk,
                         int private_key, salts_crypto_key **output) {
    int status;
    if (jwk) {
        void *key = NULL;
        jws_pkey_type_t type;
        cjwt_code_t result = jws_jwk_to_pkey(jwk, &key, &type);
        *output = key;
        return result;
    }
    status = salts_crypto_key_from_pem(data, size, private_key, output);
    return status == SALTS_CRYPTO_OK ? CJWTE_OK : status == SALTS_CRYPTO_ENOMEM ?
        CJWTE_OUT_OF_MEMORY : CJWTE_SIGNATURE_INVALID_KEY;
}

cjwt_code_t jws_verify_signature(const cjwt_t *jwt, const struct sig_input *in) {
    salts_crypto_hash hash;
    salts_crypto_signature scheme;
    salts_crypto_key_kind kind;
    salts_crypto_key *owned = NULL;
    const salts_crypto_key *key;
    cjwt_code_t result = CJWTE_SIGNATURE_VALIDATION_FAILED;
    int status;
    if (!jwt || !in) return CJWTE_INVALID_PARAMETERS;
    if (!algorithm(jwt->header.alg, &hash, &scheme, &kind)) return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
    if (!scheme) {
        uint8_t mac[64]; size_t size = 0;
        if (in->key.len > INT_MAX) return CJWTE_KEY_TOO_LARGE;
        if (salts_crypto_hmac(hash, in->key.data, in->key.len, in->full.data, in->full.len,
              mac, sizeof(mac), &size) == SALTS_CRYPTO_OK && size == in->sig.len &&
            salts_crypto_equal(mac, in->sig.data, size) == SALTS_CRYPTO_OK) result = CJWTE_OK;
        salts_crypto_clear(mac, sizeof(mac)); return result;
    }
    key = in->pkey;
    if (!key) {
        if (!in->key.data || !in->key.len) return CJWTE_SIGNATURE_MISSING_KEY;
        if (jwt->header.alg == alg_eddsa && in->key.len == 57) {
            salts_crypto_bytes pub = {in->key.data, in->key.len}, empty = {0};
            status = salts_crypto_key_from_ed(SALTS_CRYPTO_KEY_ED448, pub, empty, &owned);
            if (status != SALTS_CRYPTO_OK) return CJWTE_SIGNATURE_INVALID_KEY;
        } else {
            result = jws_load_key(in->key.data, in->key.len, NULL, 0, &owned);
            if (result != CJWTE_OK) return result;
        }
        key = owned;
    }
    if (!key_matches(key, kind)) result = CJWTE_SIGNATURE_INVALID_KEY;
    else result = salts_crypto_key_verify(key, scheme, hash, in->full.data, in->full.len,
        in->sig.data, in->sig.len) == SALTS_CRYPTO_OK ? CJWTE_OK : CJWTE_SIGNATURE_VALIDATION_FAILED;
    salts_crypto_key_destroy(owned);
    return result;
}

cjwt_code_t jws_sign(const cjwt_alg_t alg, const uint8_t *full, size_t full_len,
    const uint8_t *key_data, size_t key_len, uint8_t **sig, size_t *sig_len) {
    salts_crypto_hash hash;
    salts_crypto_signature scheme;
    salts_crypto_key_kind kind;
    salts_crypto_key *key = NULL;
    uint8_t mac[64], *output = NULL;
    size_t size = 0, capacity;
    cjwt_code_t result = CJWTE_SIGNATURE_INVALID_KEY;
    if (!sig || !sig_len || (!full && full_len) || (!key_data && key_len)) return CJWTE_INVALID_PARAMETERS;
    *sig = NULL; *sig_len = 0;
    if (!algorithm(alg, &hash, &scheme, &kind)) return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
    if (!scheme) {
        if (key_len > INT_MAX) return CJWTE_KEY_TOO_LARGE;
        if (salts_crypto_hmac(hash, key_data, key_len, full, full_len, mac, sizeof(mac), &size) != SALTS_CRYPTO_OK)
            goto done;
        output = malloc(size);
        if (!output) { result = CJWTE_OUT_OF_MEMORY; goto done; }
        memcpy(output, mac, size);
    } else {
        if (alg == alg_eddsa && key_len == 57) {
            salts_crypto_bytes priv = {key_data, key_len}, empty = {0};
            if (salts_crypto_key_from_ed(SALTS_CRYPTO_KEY_ED448, empty, priv, &key) != SALTS_CRYPTO_OK) goto done;
        } else {
            result = jws_load_key(key_data, key_len, NULL, 1, &key);
            if (result != CJWTE_OK) goto done;
        }
        result = CJWTE_SIGNATURE_INVALID_KEY;
        if (!key_matches(key, kind)) goto done;
        capacity = salts_crypto_key_signature_size(key);
        output = malloc(capacity);
        if (!output) { result = CJWTE_OUT_OF_MEMORY; goto done; }
        if (salts_crypto_key_sign(key, scheme, hash, full, full_len, output, capacity, &size) != SALTS_CRYPTO_OK) {
            salts_crypto_clear(output, capacity); goto done;
        }
    }
    *sig = output; output = NULL; *sig_len = size; result = CJWTE_OK;
done:
    salts_crypto_clear(mac, sizeof(mac)); free(output); salts_crypto_key_destroy(key);
    return result;
}
