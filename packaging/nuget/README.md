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

## Producer dependencies

- Salts.Native
- SaltsUtils.Native
- CHttp.Native
- TurboDB.Native (Orm)

The release pipeline resolves the latest stable producer packages, uses the same versions for every platform, and records the selected versions in each SDK manifest. FlowMQ and TurboRaft qualify the optional Cluster build but are not dependencies of the installed base SDK targets.

The package is built from released producer SDKs and shared vcpkg binary cache artifacts; producer source builds are not used in the release pipeline.
