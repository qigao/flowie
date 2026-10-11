#include <tinytest.h>
#include "cjwt/cjwt.h"
#include <stdlib.h>
#include <string.h>
#include "../../vendor/cjwt/tests/salts_vectors.h"

spec("cjwt Salts backend interoperability") {
    it("verifies independent JWS vectors using PEM and public/private JWK imports") {
        size_t i, k;
        for (i = 0; i < sizeof(jws_vectors)/sizeof(jws_vectors[0]); ++i) {
            uint32_t options = i < 3 ? OPT_ALLOW_ONLY_HS_ALG : 0;
            cjwt_t *decoded = NULL;
            check_equal(cjwt_decode(jws_vectors[i].token, strlen(jws_vectors[i].token), options,
                (const uint8_t *)jws_vectors[i].public_key, strlen(jws_vectors[i].public_key),
                0, 0, &decoded), CJWTE_OK);
            check_not_null(decoded);
            if (decoded) check_equal(decoded->iss, "migration");
            cjwt_destroy(decoded);
            for (k = 0; k < 2; ++k) {
                cjwt_jwk_t *jwk = NULL;
                decoded = NULL;
                check_equal(cjwt_jwk_parse(k ? jws_vectors[i].private_jwk : jws_vectors[i].public_jwk, &jwk), CJWTE_OK);
                /* The existing JWS JWK API admits asymmetric keys only. */
                if (i < 3) {
                    check_equal(cjwt_decode_with_jwk(jws_vectors[i].token, strlen(jws_vectors[i].token),
                        options, jwk, 0, 0, &decoded), CJWTE_SIGNATURE_UNSUPPORTED_ALG);
                    check_null(decoded); cjwt_jwk_destroy(jwk); continue;
                }
                check_equal(cjwt_decode_with_jwk(jws_vectors[i].token, strlen(jws_vectors[i].token),
                    options, jwk, 0, 0, &decoded), CJWTE_OK);
                check_not_null(decoded);
                if (decoded) check_equal(decoded->iss, "migration");
                cjwt_destroy(decoded); cjwt_jwk_destroy(jwk);
            }
        }
    }

    it("signs with every supported JWS algorithm and rejects a changed signature") {
        size_t i;
        for (i = 0; i < sizeof(jws_vectors)/sizeof(jws_vectors[0]); ++i) {
            uint32_t options = i < 3 ? OPT_ALLOW_ONLY_HS_ALG : 0;
            cjwt_t jwt = {0}, *decoded = NULL;
            char *token = NULL, *signature;
            jwt.header.alg = jws_vectors[i].alg; jwt.iss = "migration";
            check_equal(cjwt_encode(&jwt, (const uint8_t *)jws_vectors[i].private_key,
                strlen(jws_vectors[i].private_key), &token), CJWTE_OK);
            check_not_null(token);
            if (!token) continue;
            check_equal(cjwt_decode(token, strlen(token), options, (const uint8_t *)jws_vectors[i].public_key,
                strlen(jws_vectors[i].public_key), 0, 0, &decoded), CJWTE_OK);
            cjwt_destroy(decoded); decoded = NULL;
            signature = strrchr(token, '.') + 1;
            *signature = *signature == 'A' ? 'B' : 'A';
            check_not_equal(cjwt_decode(token, strlen(token), options, (const uint8_t *)jws_vectors[i].public_key,
                strlen(jws_vectors[i].public_key), 0, 0, &decoded), CJWTE_OK);
            check_null(decoded);
            cjwt_destroy(decoded); free(token);
        }
    }

}
