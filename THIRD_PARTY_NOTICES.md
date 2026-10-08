# Third-Party Notices

Flowie first-party code is licensed under the Apache License 2.0.

| Component | Repository path | Upstream license | Notes |
| --- | --- | --- | --- |
| cjwt | `vendor/cjwt/` | Apache-2.0 | See `vendor/cjwt/LICENSES/Apache-2.0.txt`. |
| CRoaring | `vendor/croar/` | Apache-2.0 OR MIT | License notices are embedded in the amalgamated source. |
| Monocypher | `vendor/monocypher/` | BSD-2-Clause OR CC0-1.0 | License notices are embedded in the upstream source. |
| SQLite Lemon parser generator | `tools/lemon/` | Public-domain dedication | The source headers explicitly disclaim copyright. |

Package-manager dependencies retain their respective upstream licenses.

CI uses [sccache](https://github.com/mozilla/sccache/tree/v0.16.0) v0.16.0 and
[Mozilla-Actions/sccache-action](https://github.com/Mozilla-Actions/sccache-action/tree/v0.0.11)
v0.0.11, both Apache-2.0, without local modifications. They are build tools and are not
included in Flowie's runtime packages.
