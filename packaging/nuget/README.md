# Flowie.Native

Prebuilt Flowie 1.0.4 native SDK package.

## Included SDK

- `sdk/linux-x64`
- `sdk/windows-x64`
- `sdk/android-arm64-v8a`
- CMake package: `Flowie`
- Exported targets:
  - `Flowie::Protocol`
  - `Flowie::Client`
  - `Flowie::Flowie`

## Producer dependencies

- Salts.Native
- SaltsUtils.Native
- CHttp.Native
- TurboDB.Native 2.3.4 or newer (Orm)

The release pipeline resolves published producer packages (including the Salts 2.3, SaltsUtils 4.3 and CHttp 2.1 release candidates), uses the same resolved versions for every platform, and records them in each SDK manifest. TurboRaft-backed Cluster is experimental and disabled by default. Ordinary CI and SDK releases do not restore FlowMQ or TurboRaft, build Cluster, or require its qualification. Experimental Cluster remains available through explicit source-build opt-in with compatible SDKs.

Flowie 1.0.4 uses GmSSL. Rebuild native consumers with a coherent SDK family; do not combine these binaries with the retired OpenSSL dependency family. TurboDB 2.3.4 excludes TidesDB/TidesSQL from all published SDKs. CMake discovery imposes no version constraints, including for RC SDKs; consumers must select ABI-compatible SDKs exposing the required APIs. Package restoration resolves compatible published releases. Restore into a fresh SDK directory to avoid retaining files from older packages. Remaining Flowie release blockers are recorded in `flowie/RELEASE_1.0.4.md`.

The package is built from released producer SDKs and shared vcpkg binary cache artifacts; producer source builds are not used in the release pipeline.

The shared C11/C++17 installed-package tests live in `tests/installed-sdk`. Linux and Windows
run them through CTest; Android compiles and links them. They call all three exported
libraries to verify link/load closure rather than only checking header layouts.
Native test CI invokes their versioned user presets with the root Flowie vcpkg manifest enabled.


## Windows PostgreSQL Plugin: explicit trusted DLL directories

The optional TurboDB PostgreSQL driver is a **separate Plugin**, not a
built-in Flowie transport or something automatically loaded by linking
`Flowie::Flowie`. Within one ABI-coherent set of installed native SDKs, its
absolute path and required runtime DLL locations are:

| Exact installed SDK root | Required files |
| --- | --- |
| `TURBODB_ROOT` | `lib/turbodb/drivers/turbodb_driver_postgresql.dll`, `bin/turbo_orm.dll`, `bin/libpq.dll` |
| `SALTS_ROOT` | `bin/cnet.dll`, `bin/salts.dll` |

Salts CMeta deliberately uses
`LoadLibraryExW(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`
for an explicit Plugin path. Unlike an ordinary `LoadLibraryExW(..., 0)`
probe, this **does not use arbitrary PATH entries** to find the PostgreSQL
Plugin's transitive dependencies. Merely setting `PATH` or prechecking that
the driver DLL exists is **not** a valid admission test.

The Windows application/host must resolve the exact, trusted, absolute SDK
paths and validate their files before calling `AddDllDirectory` for the
**TurboDB bin** and **Salts bin** directories. Retain both returned directory
cookies while the ORM runtime, Plugin leases, and any borrowed driver
connections remain live; after explicitly quiescing/releasing all of them,
call `orm_runtime_close`, `orm_runtime_release`, and only then
`RemoveDllDirectory`. These directory registrations affect the current
process's dynamic-loader policy and should be managed by one authorized host
owner, not opportunistically changed inside Plugin callbacks.

Load only the exact absolute PostgreSQL DLL with the *public* TurboDB
`Orm::C` API: `orm_runtime_load_driver` with
`expected_driver_id = "postgresql"`. Check
`orm_runtime_driver_info` for the expected canonical ID and ABI.
Any missing DLL/import, invalid Plugin manifest, ABI mismatch, load/start
failure, or incomplete unload remains a **hard error**. Never silently
search alternate directories, substitute the runner's PostgreSQL client,
downgrade Salts/GmSSL, or try a different provider/module.

The executable
[`tests/installed-sdk/consumer_postgresql_plugin.c`](../../tests/installed-sdk/consumer_postgresql_plugin.c)
is the runnable public-ABI acceptance contract. It loads, verifies, and
unloads the PostgreSQL driver from the *packed*, independently restored
Windows SDK—not from Flowie source-tree targets or the PATH-only loader.
[`native-sdk-pipeline.yml`](../../.github/workflows/native-sdk-pipeline.yml)
runs the dedicated
`flowie_installed_postgresql_plugin` CTest followed by the full C11/C++17
consumer suite. Corrected execution evidence:
[Flowie CI #40303712936](https://github.com/qigao/flowie/actions/runs/40303712936)
(Windows installed CTest 3/3 PASS at source commit `0914445e`).

This test only establishes *Plugin admission, identity, ABI and lifecycle*
without a running PostgreSQL server. It does not prove a live PostgreSQL
transaction, distributed Owner fencing, or end-to-end exactly-once delivery.
The current changes are on Draft PR #73, **not a release or an instruction to
switch the production dependency**.
