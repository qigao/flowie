# Flowie.Native

Prebuilt Flowie 1.0.5 native SDK package.

## Included SDK

- `sdk/linux-x64`
- `sdk/windows-x64`
- `sdk/android-arm64-v8a`
- CMake package: `Flowie`
- Exported targets:
  - `Flowie::Protocol`
  - `Flowie::Client`
  - `Flowie::Flowie`

The standalone control-plane executable is outside this SDK package and is not
built by the native package qualification jobs.

## Producer dependencies

- Salts.Native
- SaltsUtils.Native
- CHttp.Native
- TurboDB.Native 2.3.4 or newer (Orm)

The release pipeline resolves published producer packages (including the Salts 2.3, SaltsUtils 4.3 and CHttp 2.1 release candidates), uses the same resolved versions for every platform, and records them in each SDK manifest. TurboRaft-backed Cluster is experimental and disabled by default. Ordinary CI and SDK releases do not restore FlowMQ or TurboRaft, build Cluster, or require its qualification. Experimental Cluster remains available through explicit source-build opt-in with compatible SDKs.

Flowie 1.0.5 uses GmSSL and is qualified with Salts.Native 2.3.0-rc.16,
SaltsUtils.Native 4.3.0-rc.11 and CHttp.Native 2.1.0-rc.6. Rebuild native
consumers with this coherent SDK family; do not combine these binaries with the
retired OpenSSL dependency family. TurboDB 2.3.4 excludes TidesDB/TidesSQL from
all published SDKs. CMake discovery imposes no version constraints, including
for RC SDKs; consumers must select ABI-compatible SDKs exposing the required
APIs. Package restoration resolves compatible published releases. Restore into
a fresh SDK directory to avoid retaining files from older packages.

The package is built from released producer SDKs and shared vcpkg binary cache artifacts; producer source builds are not used in the release pipeline.

The shared C11/C++17 installed-package tests live in `tests/installed-sdk`. Linux and Windows
run them through CTest; Android compiles and links them. They call all three exported
libraries to verify link/load closure rather than only checking header layouts.
Native test CI invokes their versioned user presets with the root Flowie vcpkg manifest enabled.
