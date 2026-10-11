#define TINYTEST_NO_MAIN
#include "tinytest.h"
#include "cjwt/cjwt.h"
#include "jws_salts_vectors.h"

#include <stdlib.h>
#include <string.h>

spec("Flowie JWT Salts Crypto compatibility") {
  static cjwt_t *decoded;
  static cjwt_jwk_t *jwk;
  static char *encoded;

  before_each() { decoded = NULL; jwk = NULL; encoded = NULL; }
  after_each() { cjwt_destroy(decoded); cjwt_jwk_destroy(jwk); free(encoded); }

  it("verifies independent HMAC RSA ECDSA and EdDSA vectors with PEM and JWK keys") {
    for (size_t i = 0u; i < sizeof(jws_vectors) / sizeof(jws_vectors[0]); ++i) {
      const uint32_t options = i < 3u ? OPT_ALLOW_ONLY_HS_ALG : 0u;
      check_equal(cjwt_decode(jws_vectors[i].token, strlen(jws_vectors[i].token), options,
          (const uint8_t *)jws_vectors[i].public_key, strlen(jws_vectors[i].public_key),
          0, 0, &decoded), CJWTE_OK);
      check_not_null(decoded);
      check_equal(decoded->header.alg, jws_vectors[i].alg);
      check_equal(decoded->iss, "migration");
      cjwt_destroy(decoded); decoded = NULL;
      for (size_t private_key = 0u; private_key < 2u; ++private_key) {
        check_equal(cjwt_jwk_parse(private_key ? jws_vectors[i].private_jwk :
                                                 jws_vectors[i].public_jwk, &jwk), CJWTE_OK);
        const cjwt_code_t status = cjwt_decode_with_jwk(jws_vectors[i].token,
            strlen(jws_vectors[i].token), options, jwk, 0, 0, &decoded);
        /* The existing JWK admission surface is asymmetric only. */
        check_equal(status, i < 3u ? CJWTE_SIGNATURE_UNSUPPORTED_ALG : CJWTE_OK);
        if (i < 3u) check_null(decoded);
        else { check_not_null(decoded); check_equal(decoded->header.alg, jws_vectors[i].alg); }
        cjwt_destroy(decoded); decoded = NULL;
        cjwt_jwk_destroy(jwk); jwk = NULL;
      }
    }
  }

  it("signs every supported JWS algorithm and rejects modified signatures") {
    for (size_t i = 0u; i < sizeof(jws_vectors) / sizeof(jws_vectors[0]); ++i) {
      const uint32_t options = i < 3u ? OPT_ALLOW_ONLY_HS_ALG : 0u;
      cjwt_t token = {0};
      token.header.alg = jws_vectors[i].alg;
      token.iss = "migration";
      check_equal(cjwt_encode(&token, (const uint8_t *)jws_vectors[i].private_key,
                              strlen(jws_vectors[i].private_key), &encoded), CJWTE_OK);
      check_not_null(encoded);
      check_equal(cjwt_decode(encoded, strlen(encoded), options,
          (const uint8_t *)jws_vectors[i].public_key, strlen(jws_vectors[i].public_key),
          0, 0, &decoded), CJWTE_OK);
      cjwt_destroy(decoded); decoded = NULL;
      char *signature = strrchr(encoded, '.');
      check_not_null(signature);
      ++signature;
      check_not_equal(*signature, '\0');
      *signature = *signature == 'A' ? 'B' : 'A';
      check_equal(cjwt_decode(encoded, strlen(encoded), options,
          (const uint8_t *)jws_vectors[i].public_key, strlen(jws_vectors[i].public_key),
          0, 0, &decoded), CJWTE_SIGNATURE_VALIDATION_FAILED);
      check_null(decoded);
      free(encoded); encoded = NULL;
    }
  }
}
