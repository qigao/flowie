# Flowie.Native

Prebuilt Flowie 1.1.0 native SDK package.

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
- TurboDB.Native (Orm)

The release pipeline resolves published producer packages (including the Salts 2.3, SaltsUtils 4.3 and CHttp 2.1 release candidates), uses the same resolved versions for every platform, and records them in each SDK manifest. FlowMQ and TurboRaft qualify the optional Cluster build but are not dependencies of the installed base SDK targets.

Flowie 1.1 uses GmSSL. Rebuild native consumers with a coherent SDK family; do not combine these binaries with the retired OpenSSL dependency family. TurboDB 2.3.3 removes the TidesDB ORM driver but its published package still contains TidesSQL/TidesDB, unlike the existing custom deployment build. A producer package matching the disabled-TidesSQL requirement remains a deployment release blocker; see `flowie/RELEASE_1.1.0.md`.

The package is built from released producer SDKs and shared vcpkg binary cache artifacts; producer source builds are not used in the release pipeline.

The shared C11/C++17 installed-package tests live in `tests/installed-sdk`. Linux and Windows
run them through CTest; Android compiles and links them. They call all three exported
libraries to verify link/load closure rather than only checking header layouts.
