# Third-Party Notices

Flowie first-party code is licensed under the Apache License 2.0.

| Component | Repository path | Upstream license | Notes |
| --- | --- | --- | --- |
| cjwt / JOSE provider | Installed `CHttp::JWT` target | Apache-2.0 and provider notices | Supplied by the Chttp SDK; see its installed licenses under `share/licenses/Chttp/`. |
| CRoaring | `vendor/croar/` | Apache-2.0 OR MIT | License notices are embedded in the amalgamated source. |
| OpenWTL / OpenATL / WTL | Windows Workbench build dependency | Apache-2.0 / MS-PL | Unmodified OpenWTL archive selected and SHA-256 verified from `.env`; upstream licenses installed under `share/licenses/Flowie/OpenWTL/`. |
| Monocypher | `vendor/monocypher/` | BSD-2-Clause OR CC0-1.0 | License notices are embedded in the upstream source. |
| SQLite Lemon parser generator | Restored `Qigao.Lemon.Binary` build tool | Public-domain dedication | Supplied by the selected vcpkg-cache release. |

Package-manager dependencies retain their respective upstream licenses.

Flowie consumes cjwt through the public `<cjwt/cjwt.h>` API from
[qigao/chttp](https://github.com/qigao/chttp). Its implementation and private
cryptographic providers remain in that SDK; Flowie no longer carries a second
vendor copy. JWT/JWKS authentication retains its configured signing-algorithm,
key ID, issuer, audience and claim checks.

CI uses [sccache](https://github.com/mozilla/sccache/tree/v0.16.0) v0.16.0 and
[Mozilla-Actions/sccache-action](https://github.com/Mozilla-Actions/sccache-action/tree/v0.0.11)
v0.0.11, both Apache-2.0, without local modifications. They are build tools and are not
included in Flowie's runtime packages.
