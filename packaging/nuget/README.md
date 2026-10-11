# Flowie.Native

Prebuilt Flowie 1.0.2 native SDK package.

## Included SDK

- `sdk/linux-x64`
- `sdk/windows-x64`
- `sdk/android-arm64-v8a`
- CMake package: `Flowie`
- Exported targets:
  - `Flowie::Protocol`
  - `Flowie::Client`
  - `Flowie::Flowie`

## Exact producer baseline

- Salts.Native 1.8.21
- SaltsUtils.Native 4.1.18
- CHttp.Native 1.1.10
- TurboDB.Native 2.1.0 / Orm 2.1.0

FlowMQ 1.2.0 and TurboRaft 0.3.1 are used to qualify Flowie's optional Cluster build, but are not dependencies of the installed base SDK targets.

The package is built from released producer SDKs and shared vcpkg binary cache artifacts; producer source builds are not used in the release pipeline.
