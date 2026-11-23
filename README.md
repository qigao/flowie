# Flowie

**MQTT server/client infrastructure built on the Salts ecosystem.**

Flowie owns MQTT protocol behavior, broker core state, sessions, subscriptions, retained messages, authentication boundaries, and the server/client lifecycle. It reuses Salts for networking/runtime semantics and CHTTP for HTTP/WebSocket transport integration instead of introducing a second systems runtime.

**Tags:** C11 · C++17 · MQTT · broker · pub-sub · WebSocket · TLS · networking · Salts · CHTTP

Windows builds include [Flowie Workbench](flowie/client/app/readme.md), an OpenWTL
desktop client for MQTT, HTTP/HTTPS, JSON-RPC, raw TCP and serial testing. Up to eight
independent tabs can share Collections and keep separate connections and histories.
Run `bin/flowie_mqtt_workbench.exe` from the native build
or normal installation directory. Its selected GUI dependency is recorded in `.env`.

See the [documentation index](docs/README.md) for guides, deployment and architecture decisions.

## Built on Salts

Flowie is intentionally layered on the shared Salts foundation:

Source builds use the SDK versions selected in `.env`. The installed Flowie package
records the exact versions it links. Chttp supplies the required App and shared JWT interfaces.

Flowie consumes the split `SaltsBase`, `CMeta`, `SaltsUtils` and `SaltsCNet`
packages. Core facilities use headers such as `<core/error.h>`,
`<threads/threads.h>` and `<memory/buffer.h>`; reflection and execution use
`<cmeta/...>` and `<cflow/...>`. Rebuild Flowie against one consistent SDK set
after upgrading these dependencies.

CNet accepts retained buffers for stream sends. Flowie's byte-oriented
send paths copy into an immutable `mem_buffer_t`, release their local reference
after admission, and let CNet retain the payload until terminal completion.
This preserves caller-buffer and mailbox reuse, including after a client
timeout, without changing Flowie's public send API. Existing packet limits and
bounded queues still govern admission; allocation failures return an error
(or stop the server progress owner before consuming its pending command).
Retained scatter/gather sends continue to use `cnet_send_slicev` directly.
Flowie's public ORM header also references CFlow type descriptors, so
`Flowie::Flowie` propagates `CMeta::CFlow` to consumers.

- **Salts::CNet** provides TCP/TLS transport and explicit connection/session progress.
- **Salts Core / Coroutine / runtime primitives** provide common systems facilities.
- **SaltsUtils** provides JSON/YAML/command-line/LTV and related parser components used by broker/server features.
- **CHTTP** provides HTTP and WebSocket client/server infrastructure.
- **TurboDB ORM** provides database integration through the configured installed profile.
- experimental server/cluster mode integrates with TurboRaft 0.3.2 and Salts CNet through their public packages; it requires explicit opt-in.

Flowie does not push MQTT-specific state back into Salts. Dependency direction remains one-way.

## Ecosystem role

```text
Salts
  ├── salts-utils (including DataBind)
  └── salts-net
        ↓
      CHTTP
        ↓
      Flowie
        ↓
 downstream product / workflow adapters
```

Flowie is an **application/service infrastructure** layer. Higher-level products may consume Flowie through adapters, but Flowie's public API does not depend on those orchestration products.

## Ownership boundary

Flowie owns:

- MQTT wire protocol;
- broker core state;
- client/server behavior;
- sessions;
- subscriptions;
- retained messages;
- authentication boundaries;
- MQTT transport adaptation;
- optional broker clustering integration.

Flowie does **not** own:

- generic workflow execution;
- product-specific business graphs;
- upper-layer orchestration types;
- hidden alternate network runtimes.

A downstream workflow or product adapter performs the mapping from MQTT messages into its own business/event model.


### CMeta / RAII authority split

Flowie follows the ecosystem ownership contract tracked by [Salts #878](https://github.com/qigao/salts/issues/878) and [Flowie #57](https://github.com/qigao/flowie/issues/57):

```text
CMeta / generated producers
  = native managed-value lifecycle

CNet / CHTTP
  = transport + asynchronous connection/request lifetime

Flowie
  = MQTT broker/session/subscription/retained-message lifetime

Plugin (when used)
  = explicit outer module/provider lifetime
```

MQTT sessions, subscriptions, retained publications, connection shutdown, and in-flight CNet operations are domain/runtime lifetime and are not mechanically converted to lexical CMeta RAII. Conversely, reflected/generated native values must not introduce a Flowie-private `OWNED/SHARED/BORROWED` semantic vocabulary or cleanup registry.

## Dependency direction

```text
downstream product adapters (optional)
              ↓
        Flowie::Flowie
              ↓
 Flowie::Protocol + Salts::CNet + CHttp::Client/Server
```

The public Flowie API does not expose upper-layer orchestration types.

## 源码与构建布局

| 路径 | 职责 |
| --- | --- |
| `flowie/protocol/` | MQTT 编解码与报文视图 |
| `flowie/transport/` | 连接、发送批次、执行绑定与 I/O 策略 |
| `flowie/client/` | 客户端 SDK 与客户端测试 |
| `flowie/core/` | Broker、会话、协议存储与基础集群契约 |
| `flowie/cluster/` | 可选 TurboRaft/CNet 集群扩展 |
| `flowie/server/` | 服务端入口、配置与运行时组装 |
| `flowie/control/` | 控制面源码、schema、解析器与界面资源 |
| `flowie/detail/` | 不安装的跨模块内部头文件 |
| `tests/support/` | 跨模块复用的测试夹具 |
| `cmake/Flowie*.cmake` | SDK 依赖发现、运行时依赖复制与安装导出 |

各模块维护自己的 `CMakeLists.txt` 和 `tests/`；根 CMake 负责组装。
安装后的 `Flowie::Protocol`、`Flowie::Client`、`Flowie::Flowie` 与头文件名保持稳定。
目录迁移、依赖方向和验证边界见[项目结构决策](docs/ADR_PROJECT_LAYOUT.md)。

## Build and test

Windows development build:

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Profile windows-llvm-mingw-x64 -Local
cmake --preset native-release-user -DBUILD_TESTS=ON -DBUILD_TESTING=ON -DFLOWIE_BUILD_TESTS=ON
cmake --build --preset native-release-user
ctest --preset native-release-user --output-on-failure
```

The build options are listed in `CMakeOptions.cmake` and the module CMake files.

The native workflows use the same complete preset build graph and formal CTest entry points.
Linux GCC and Windows MSVC run their tests; Android arm64 performs cross-build, installation and
16 KiB ELF alignment checks. Android device execution is not implied. Successful runs produce
original SDK TAR archives, a release manifest and SHA256SUMS. Tag releases publish these files to
GitHub Releases. Distribution and dependency restoration no longer use a product-owned package
feed, package client or cache proxy.

CI generates each job's `.env` from the dependency releases committed in `.env.example`.
The matrix selects `x64-linux`, `x64-windows` or `arm64-android` through the SDK profile.
The bootstrap sets workspace-local paths for the shared checkout and installation. Android uses
the runner's `ANDROID_NDK_HOME`, and Windows copies the pinned OpenWTL version and SHA-256.
Credentials come from the runner token environment.

The shared vcpkg-cache release provides the installed dependencies and the re2c and Lemon
archives. The bootstrap restores them through the selected shared checkout and records the
returned paths in `.env`. Native jobs upload CTest logs, with Workbench window renders on Windows.
For Android, restore also records `SALTS_IDLC_ROOT` and `SALTS_IDLC_BASE_ROOT` for
the same-version host compiler and its runtime. Control schemas are generated on
the host while Android targets link the Android SDK.
Automatic branch and pull request builds ignore changes limited to CMake, JSON, and documentation.
Manual workflow dispatch can validate those changes. Version tags trigger the release workflow.

Common options include:

- `FLOWIE_BUILD_SERVER` — build the standalone MQTT server.
- `BUILD_TESTS` and `FLOWIE_BUILD_TESTS` enable module test directories. Set both
  to `ON` when configuring a full local regression run. `BUILD_TESTING` controls CTest and protocol tests.
- `FLOWIE_BUILD_CLUSTER` includes the experimental TurboRaft cluster module. See
  the cluster configuration below.
- `FLOWIE_BUILD_CONTROL` — enable optional flowie/control-plane components.

### Slate 本地构建

Windows x64 可使用 Slate 0.1.14、Ninja、CMake 和 `pkgconf` 构建。
先按上述 Windows 流程恢复 LLVM-MinGW 的 SDK。Slate 直接读取根目录的 `.env`，
进程环境中的同名变量优先，须指向同一轮恢复的 SDK。
SDK 恢复会从 `RE2C_ROOT` 写入 Slate 专用的 `FLOWIE_RE2C_ROOT`，避免 VS Code
继承其他项目的 re2c 路径。更新 re2c 后通过同一恢复流程同步这两个字段。

```powershell
slate doctor --preset release
slate build --preset release
slate test --preset release --list
slate test --preset release --build-dir build/slate-tests
```

`release` 构建 MQTT 协议库、Client 和 Broker DLL、`flowie_server`、
`flowie-control` 与 `flowie-control-data`。本地 Control 进程从源码目录读取 Dashboard 资源。
测试随所属模块注册，可按名称用
`--filter` 选择，或按标签用 `--label` 选择。
已配置匹配的 `TURBORAFT_ROOT` 时，使用 `--preset cluster` 构建集群扩展，
并使用 `slate test --preset cluster --label server/cluster` 运行集群测试。
完整测试图可用 `slate graph --preset cluster --format json --tests` 查看。

Workbench、Linux、MSVC、Android、发布打包、夜间 soak/fuzz 及外部服务资格测试
沿用上文的 CMake/CTest 流程。

## Upstream dependency versions

`.env` is the sole source of non-secret build configuration. `.env.example` is a template;
merge selected fields into `.env` while preserving credentials and machine settings.
The restore loader replaces inherited build settings with the selected file values and never
imports credentials from the file. CMake consumes the loaded environment and does not read dotenv.

The selected family is SaltsBase 0.1.8, CMeta 0.1.8, SaltsUtils 0.1.6, SaltsCNet 0.1.16,
[Chttp 0.1.2](https://github.com/qigao/chttp/releases/tag/v0.1.2),
[TurboRaft 0.3.2](https://github.com/qigao/turboraft/releases/tag/v0.3.2) and TurboDB 0.1.2,
with vcpkg-cache 0.1.3. Versions and roots are selected in `.env`:

- `SALTS_BASE_ROOT`, `CMETA_ROOT`, `SALTS_UTILS_ROOT`, `SALTS_CORE_ROOT` / `SALTS_CNET_ROOT`;
- `CHTTP_ROOT` and `TURBODB_ROOT`, plus `TURBORAFT_ROOT` for cluster builds;
- `VCPKG_CACHE_REPOSITORY_ROOT` selects the matching shared infrastructure checkout;
- `VCPKG_INSTALLED_DIR`, `RE2C_ROOT`, `LEMON_ROOT` reference its restored installations.

Flowie does not maintain copies of vcpkg or host-tool release declarations. Restore these
through the [shared vcpkg-cache entry point](https://github.com/qigao/vcpkg-cache/blob/v0.1.3/docs/restore-native-dependency.md),
then select their returned roots in `.env`. The SDK restore entry points are:

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Profile windows-llvm-mingw-x64 -Local
cmake --fresh --preset native-release-user -DBUILD_TESTS=ON -DBUILD_TESTING=ON -DFLOWIE_BUILD_TESTS=ON
cmake --build --preset native-release-user
ctest --preset native-release-user --output-on-failure -j 1
```

Restore verifies release manifests, producer CI and archive digests before committing versioned
SDK roots back to `.env`. TurboDB uses the published SDK; source-build GUI dependencies are not
part of the Flowie build graph. SDK/compiler/triplet changes require an explicit fresh configure.

Cluster builds use `-DBUILD_CLUSTER=ON -DFLOWIE_BUILD_CLUSTER=ON` and the exact
`TURBORAFT_VERSION` / `TURBORAFT_ROOT`.
TurboRaft 0.3.2 uses `TurboRaft::CNet`; no additional messaging package is required.
Its release has no Windows LLVM-MinGW archive. This profile was built from the exact
[0.3.2 source revision](https://github.com/qigao/turboraft/tree/3e311ba3e23585a75c16cea1a5d462edcf332c09)
using its existing native preset and the same SDK family; its formal CTests passed before use.
Restore verifies that the selected archives match the requested releases. Source configuration
supplies the SDK roots to CMake `find_package`, which can also search its default paths.
The installed `FlowieConfig.cmake` confines SDK lookup to its explicit paths.

## HTTP and WebSocket integration

HTTP/WebSocket support comes from the standalone [CHTTP](https://github.com/qigao/chttp) SDK:

- outbound HTTP and WebSocket client code links `CHttp::Client`;
- HTTP and WebSocket listeners link `CHttp::App`; Controller actions and ordered
  WebSocket events run on its bounded workers;
- JWT/JWKS authentication links the shared static `CHttp::JWT` target and public
  `<cjwt/cjwt.h>` header, without a second local cjwt/provider copy;
- TCP/TLS transport remains owned by Salts CNet.

See [the App migration decision](docs/ADR_CHTTP_APP_INTEGRATION.md) for lifetime, quotas and validation.

This lets MQTT-over-WebSocket share the same explicit transport, lifecycle, and error semantics as the rest of the Salts ecosystem.

## Parser and utility integration

JSON, YAML, command-line, LTV, and related parser capabilities come from the installed [SaltsUtils](https://github.com/qigao/salts-utils) SDK.

These are independent component dependencies, not evidence that Flowie owns a parser runtime.

## Run the standalone broker

```powershell
flowie_server --host 127.0.0.1 --port 1883 --transport tcp
```

WebSocket listener example:

```powershell
flowie_server --host 127.0.0.1 --port 8080 --transport ws --path /mqtt
```

TLS/WSS certificate configuration uses:

```text
SALTS_TLS_CERT_FILE
SALTS_TLS_KEY_FILE
```

Validate startup configuration without listening:

```powershell
flowie_server --check --protocol-store-driver sqlite --protocol-store-options '{"filename":":memory:"}'
```

TCP/TLS brokers can opt into multiple fixed CNet owners with `--network-workers 4`
or `network_workers: 4` in the endpoint adapter's YAML `config`. Zero or one keeps
the single-owner default. `network_command_bytes` (`--network-command-bytes`) is
an aggregate mailbox budget, divided across owners; each partition must fit one
maximum-sized packet. The default 16 MiB budget supports four owners with the
default 1 MiB packet limit. MQTT session/topic state stays on its existing executor
shard. See [network ownership and shutdown](docs/ARCHITECTURE.md#tcptls-network-owners)
for capacity, ABI, flowie/transport, and lifecycle constraints.

Connection placement accepts `--network-policy round-robin` (default) or
`--network-policy least-connections`, counting live connections and pending
handoffs. Optional `--network-cpus 2,4` binds two owners to those logical CPUs;
the list must contain one CPU per owner. Omit it to keep OS scheduling. YAML uses:

```yaml
network_workers: 2
network_policy: least-connections
network_cpus: [2, 4]
```

These keys belong inside the endpoint adapter's `config`. With `--config`, YAML
provides endpoint tuning; the CLI tuning flags do not override it. CPU IDs are
OS logical IDs on Linux/Android, and `group * 64 + processor` on Windows. Binding
is applied before network progress; unavailable CPUs fail startup. Other platforms
reject explicit binding. UDP/KCP remain single-owner. WS/WSS accepts owner counts
and round-robin/least-connections placement through CHttp, with one aggregate
Flowie wrapper mailbox; explicit CPU binding remains unsupported for WS/WSS.
No throughput improvement is claimed without measurement.

The C API copies the same settings during `flowie_endpoint_core_create`:

```c
flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
config.host = "127.0.0.1";
config.port = 1883;
config.network_workers = 2;
config.network_policy = FLOWIE_NETWORK_LEAST_CONNECTIONS;
config.network_cpu_count = 2;
config.network_cpus[0] = 2;
config.network_cpus[1] = 4;
```

Unknown policies or CPU-list length mismatches return `SALTS_EINVAL` at creation.
Explicit CPU binding occurs at `flowie_endpoint_core_start`; check its result
before accepting the endpoint as running. Rebuild C API consumers for the expanded
configuration struct; existing zero-initialized settings preserve the defaults.

## CNet connection management and Server composition

TCP/TLS owners use the Manager API in `Salts::CNet` for bounded attachment reservation,
adoption and retirement. Each owner initializes, advances and destroys its
manager on its worker thread. CNet owns flowie/transport terminal callbacks; peer
storage is reused only by the manager recycle callback after callbacks return.
MQTT context and retained send ownership remain with their existing owners.

The multi-owner listener reserves a generation-checked `cnet_handoff` ticket
before accepting. Publication moves the socket to its final owner's bounded
inbox. On failure the listener closes the socket and releases the ticket;
on success ownership transfers even if wake fails. Tickets remain charged
through peer recycling. Round-robin and least-connections placement use the
handoff snapshot as the single source of pending plus active occupancy.

Shutdown seals inboxes before owners drain queued sockets and stop CNet.
Managers recycle records and are destroyed on their worker threads. Stop joins
owners and the listener (including final wake) before destroy frees inboxes and
clients. Timeout ownership, stale public handles, MQTT semantics, UDP/KCP and
CHTTP WebSocket flowie/protocol ownership are preserved.

The standalone Server uses the split SDK set described above and GmSSL for TLS.
It composes Security, Repository and Endpoint through CMeta ACE and uses
Interceptors for HTTP Auth/ACL request admission and cleanup. `--check` assembles the same graph without opening the
listener. Startup failure rolls back before publication; shutdown releases the
Endpoint before its dependencies. WS/WSS placement remains owned by CHttp.
See [the ACE/CNet decision](docs/ADR_ACE_CNET_23_SERVER_CLIENT.md) for ownership,
validation and rollback boundaries. No performance gain is claimed.

## CMake consumption

```cmake
find_package(Flowie CONFIG REQUIRED)
target_link_libraries(my_broker PRIVATE Flowie::Flowie)
```

Point `CMAKE_PREFIX_PATH` or `Flowie_DIR` at the Flowie installation and set the
dependency roots and vcpkg triplet listed in [SDK configuration](#upstream-dependency-versions).
The package requires the split SDK versions recorded at install time, including
Chttp App/JWT, plus GmSSL from the selected vcpkg installation.

Consumers may also use the narrower exported targets:

- `Flowie::Protocol`
- `Flowie::Client`

`Flowie::Transport` is an implementation detail and is not installed/exported as a public target.

## Design principles

- **Single systems foundation.** Reuse Salts networking/runtime semantics instead of embedding a second event loop.
- **Protocol ownership stays local.** MQTT semantics belong to Flowie, not Salts.
- **Upper-layer independence.** Flowie does not depend on workflow/business products.
- **Explicit package boundaries.** CHTTP, SaltsUtils, and TurboDB are consumed through installed public packages.
- **Fail fast.** Missing dependencies do not trigger hidden fallback implementations.
- **Explicit lifecycle.** Connection/session ownership and shutdown remain visible to the caller/runtime owner.

## Relationship to TurboFlow

TurboFlow may consume Flowie through an adapter, but the two repositories own different concerns:

```text
Flowie
  MQTT protocol, broker state, sessions, subscriptions

TurboFlow
  graph/workflow/business execution and provider orchestration
```

MQTT receive can be modeled as a Source and MQTT send as a Sink in an upper-layer graph, but MQTT is not TurboFlow's internal data model.

---

**Salts provides the systems runtime. CHTTP provides HTTP/WebSocket infrastructure. Flowie provides MQTT semantics.**
