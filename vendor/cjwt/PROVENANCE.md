# cjwt provenance

- Upstream: https://github.com/xmidt-org/cjwt
- Version: 2.3.0
- Imported from local TurboHTTP commit: `38f1e389b3f94909db6cb2482a8cbc16522e7e4f`
- License: Apache-2.0 (the SPDX headers remain in every imported source file)

This private Flowie copy contains only the JWS/JWK surface used by the control-plane JWT
authenticator. Local changes replace TurboParser with Salts JsonParser, replace TurboCrypto
helpers with SaltsUtils Crypto, and deliberately reject the unused JWE code path.
The JWS adapter and independent JWS vectors are synchronized from
`qigao/chttp` commit `a713d02bdbb19fa6f74bfb2293065d0771739ca0`.
The adapted JWS regression lives in `control/tests/test_flowie_control_jws_backend.c`.
