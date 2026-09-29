# Flowie.Native

Prebuilt Flowie 1.0.1 native SDK package.

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

- Salts.Native 1.8.3
- SaltsUtils.Native 4.1.3
- CHttp.Native 1.1.5
- TurboDB.Native 1.0.1 / Orm 2.1.0

FlowMQ 1.1.1 and TurboRaft 0.2.0 are used to qualify Flowie's optional Cluster build, but are not dependencies of the installed base SDK targets.

The package is built from released producer SDKs and shared vcpkg binary cache artifacts; producer source builds are not used in the release pipeline.
