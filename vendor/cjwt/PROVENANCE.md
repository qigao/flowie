# cjwt provenance

- Upstream: https://github.com/xmidt-org/cjwt
- Version: 2.3.0
- Imported from local TurboHTTP commit: `38f1e389b3f94909db6cb2482a8cbc16522e7e4f`
- License: Apache-2.0 (the SPDX headers remain in every imported source file)

This private Flowie copy contains only the JWS/JWK surface used by the control-plane JWT
authenticator. Local changes replace TurboParser with Salts JsonParser, replace TurboCrypto
helpers with OpenSSL primitives, and deliberately reject the unused JWE code path.
