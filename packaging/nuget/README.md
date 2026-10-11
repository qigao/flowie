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

- Salts.Native 2.3.0-rc.10
- SaltsUtils.Native 4.3.0-rc.8
- CHttp.Native 2.1.0-rc.3
- TurboDB.Native 2.3.4 / Orm 2.3.4

Cluster remains disabled in the release build. Its optional FlowMQ and TurboRaft
dependencies are outside this SDK migration and are not qualified by this pipeline.

The package is built from released producer SDKs and shared vcpkg binary cache artifacts; producer source builds are not used in the release pipeline.

CMake dependency discovery accepts the SDKs at the configured roots without version
constraints, including RC builds. The producer baseline above records the package
build inputs; consumers are responsible for choosing ABI-compatible SDKs.
