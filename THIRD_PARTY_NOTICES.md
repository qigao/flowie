# Third-Party Notices

Flowie first-party code is licensed under the Apache License 2.0.

| Component | Repository path | Upstream license | Notes |
| --- | --- | --- | --- |
| cjwt / JOSE provider | Installed `CHttp::JWT` target | Apache-2.0 and provider notices | Supplied by the Chttp SDK; see its installed licenses under `share/licenses/Chttp/`. |
| CRoaring | `vendor/croar/` | Apache-2.0 OR MIT | License notices are embedded in the amalgamated source. |
| OpenWTL / OpenATL / WTL | Windows Workbench build dependency | Apache-2.0 / MS-PL | Unmodified OpenWTL archive selected and SHA-256 verified from `.env`; upstream licenses installed under `share/licenses/Flowie/OpenWTL/`. |
| Monocypher | `vendor/monocypher/` | BSD-2-Clause OR CC0-1.0 | License notices are embedded in the upstream source. |
| SQLite Lemon parser generator | Restored `Qigao.Lemon.Binary` build tool | Public-domain dedication | Supplied by the selected vcpkg-cache release. |
| Zstandard | Installed `zstd::libzstd_static` target | BSD-3-Clause | Supplied by the selected vcpkg-cache release. Control uses it to inspect release package frames. |

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

## Zstandard BSD License

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
* Neither the name Facebook, nor Meta, nor the names of its contributors may
  be used to endorse or promote products derived from this software without
  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
