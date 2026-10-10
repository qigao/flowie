# Flowie

**MQTT server/client infrastructure built on the Salts ecosystem.**

Flowie owns MQTT protocol behavior, broker core state, sessions, subscriptions, retained messages, authentication boundaries, and the server/client lifecycle. It reuses Salts for networking/runtime semantics and CHTTP for HTTP/WebSocket transport integration instead of introducing a second systems runtime.

**Tags:** C11 · C++17 · MQTT · broker · pub-sub · WebSocket · TLS · networking · Salts · CHTTP

## Built on Salts

Flowie is intentionally layered on the shared Salts foundation:

Flowie uses the installed Salts 2.3.0, SaltsUtils 4.3.0, Chttp 2.1.0, and TurboDB/Orm >= 2.3.2 packages. Both the build and the exported Flowie package enforce these versions using each producer's CMake compatibility policy; Chttp requires the full `2.1.0` version. Optional cluster builds also use the installed FlowMQ and TurboRaft packages. Configure each profile root to the matching published SDK family; configuration and compilation fail if a required API is unavailable. Native CI retains its floating producer-package resolution; the exact release candidates below are the SDKs used for this refactor's Linux qualification.

Flowie uses the canonical Salts headers and symbols (`cmeta_*.h`, `cmeta_*`,
`coro.h`, and `coro_executor.h`). Rebuild Flowie and its dependent SDKs together
after the Salts public-name migration; old binaries referencing `salts_*`
symbols are incompatible. Error codes, ownership, executor placement, and MQTT
behavior are unchanged.

Salts CNet accepts retained buffers for stream sends. Flowie's byte-oriented
send paths copy into an immutable `mem_buffer_t`, release their local reference
after admission, and let CNet retain the payload until terminal completion.
This preserves caller-buffer and mailbox reuse, including after a client
timeout, without changing Flowie's public send API. Existing packet limits and
bounded queues still govern admission; allocation failures return an error
(or stop the server progress owner before consuming its pending command).
Retained scatter/gather sends continue to use `cnet_send_slicev` directly.
Flowie's public ORM header also references CFlow type descriptors, so
`Flowie::Flowie` propagates `Salts::CFlow` to consumers.

- **Salts::CNet** provides TCP/TLS transport and explicit connection/session progress.
- **Salts Core / Coroutine / runtime primitives** provide common systems facilities.
- **SaltsUtils** provides JSON/YAML/command-line/LTV and related parser components used by broker/server features.
- **CHTTP** provides HTTP and WebSocket client/server infrastructure.
- **TurboDB ORM** provides database integration through the configured installed profile.
- optional cluster mode integrates with FlowMQ and TurboRaft through their public packages.

Flowie does not push MQTT-specific state back into Salts. Dependency direction remains one-way.

### Fixed network owners and admission

Multi-owner TCP/TLS accepts use CNet's `owner_placement.h` and `handoff.h`.
Flowie maps its existing round-robin and least-connections settings to CNet's
round-robin and lowest-pressure decisions. The pressure is the inbox snapshot's
reserved + queued + taken credits; ties rotate from the next owner. Selection is
advisory, and `cnet_handoff_reserve` commits the actual admission. Established
connections retain their owner, public slot/generation and callback ordering.

Each owner has one bounded inbox whose connection and queue capacities equal
that owner's existing connection-capacity partition. The listener is the sole
producer and the fixed network worker is the sole consumer. A reservation is
released if detached accept fails. Successful publication moves the detached
socket and ticket into the inbox; rejection leaves both with the listener,
which closes the socket and releases the ticket. Taking returns queue space;
the peer keeps its credit until a real CNet CLOSED/FAILED callback. Adoption
failure closes the detached socket and returns its credit. Full owners leave
new connections in the listener backlog. No payload, TLS state or live stream
migrates between owners.

Stop seals every inbox before draining queued sockets and stopping CNet. A
timeout preserves workers, inboxes and payloads for a later stop. Destroy requires
all network workers and the accept producer to be joined, including publication
and wake tails, and every inbox to be drained. Flowie owns this lifecycle;
the CNet helpers create no threads or scheduler. Single-owner and packet/HTTP
WebSocket progress retain their existing owners.

This replaces Flowie's private accepted-socket ring and duplicate admission
counter with the existing CNet protocol. Keeping the old ring would duplicate
capacity, generation and seal rules; introducing a Component registry would
add lifecycle authority without a provider-selection requirement. CMeta/ACE
remain the declaration and composition contracts; this change adds no reflection
table, registry, Component generation, protocol READY classification or recovery
policy. MQTT sessions and acknowledgements remain owned by Flowie. Rollback
requires rebuilding Flowie against its preceding coherent SDK set; there is no
wire, persisted-data or public configuration migration.

The requested qualification SDKs are [Salts 2.3.0-rc.4](https://github.com/qigao/salts/releases/tag/v2.3.0-rc.4),
[SaltsUtils 4.3.0-rc.2](https://github.com/qigao/salts-utils/releases/tag/v4.3.0-rc.2),
[Chttp 2.1.0-rc.1](https://github.com/qigao/chttp/releases/tag/v2.1.0-rc.1) and
[TurboDB 2.3.2](https://github.com/qigao/turbodb/releases/tag/v2.3.2).
SaltsNet 1.1.0-rc.1 and TurboWasm 0.2.0 are available ecosystem SDKs, but Flowie
does not consume their APIs and does not add them as link dependencies.

External HTTPS and JWKS authenticator configuration validates trust files and
client identity through the same Chttp/CNet TLS profile used by requests,
instead of prechecking them through a separate OpenSSL TLS context. Temporary
profiles are destroyed before creation returns, and encrypted-key passwords
retain the existing secret lease and wipe protocol. Verification and identity
remain enabled. JWT signature cryptography and the independent TLS test servers
retain their existing crypto dependencies.

Linux qualification for this refactor used GCC 12.2, the published SDKs above,
and the `ci-linux-release-user` configure/build/test presets with vcpkg manifest
mode enabled. The complete Release graph built and `install-ci-linux-release-user`
installed successfully. After the server ACE refactor, CTest passed all 51 enabled
tests; the public MQTT live test remained disabled. The connection suite passed
ten consecutive runs, and the server runtime and HTTPS security suites each
passed ten consecutive runs using `--repeat until-fail:10`.

The `eu` build host runs Debian 12 with glibc 2.36. The published CNet library
requires `GLIBC_2.38`, so tests ran in an isolated Ubuntu 24.04 container with
glibc 2.39 against the same build and SDK tree. Reproduce the test runs with
`ctest --preset ci-linux-release-user --output-on-failure` and
`ctest --preset ci-linux-release-user -R '^test_flowie_connection$' --repeat until-fail:10 --output-on-failure`
in that compatible runtime. Windows, sanitizers, live services and the optional
FlowMQ/TurboRaft cluster runtime were not qualified in this run.

Server lifecycle/interceptor regressions can be selected with
`ctest --preset ci-linux-release-user -R '^(test_flowie_server_runtime|test_flowie_server_http_security|flowie_server_check_.*)$' --output-on-failure`.

The control-plane uses the Salts Component Configurator for Repository,
Identity/Session and Application assembly, rollback and reverse-order cleanup.
All dashboard pages and management content use `CHttp::App` with native CMeta
VIEW models and a bounded, serialized Jinja renderer. Fixed section includes
preserve tree, pagination and permission behavior; forms retain Flowie's CSRF
and management authority. See [App ownership and resource migration](flowie/ARCHITECTURE.md#control-plane-chttpapp-composition).
After the complete control-plane ACE/Jinja migration, the Release graph rebuilt
and installed successfully. All 51 enabled CTest tests passed; runtime,
bootstrap runtime, dashboard and HTTPS integration each passed 10 consecutive
runs in the same compatible Ubuntu 24.04 SDK container. The new rollback test
verifies Identity/Application failure cleanup and persistent bootstrap idempotency.
Select the relevant regressions with
`ctest --preset ci-linux-release-user -R '^test_flowie_control_(runtime|bootstrap_runtime|dashboard|https_integration)$' --output-on-failure`.

## Ecosystem role

The standalone executable also uses an explicit `Salts::Component` graph for
security, repository and endpoint startup/rollback. Auth/ACL HTTPS callbacks use
native-typed CMeta Interceptor chains for admission and success/error cleanup.
See [server composition and ownership](flowie/ARCHITECTURE.md#standalone-server-component-configurator-and-interceptor)
for configuration lifetime, check-only behavior and the Leader/Followers boundary.

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

## Dependency direction

```text
downstream product adapters (optional)
              ↓
        Flowie::Flowie
              ↓
 Flowie::Protocol + Salts::CNet + CHttp::Client/Server
```

The public Flowie API does not expose upper-layer orchestration types.

## Build and test

Windows development build:

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --test-dir build/Msvc --output-on-failure
```

All feature switches are declared centrally in `CMakeOptions.cmake`.

Common options include:

- `FLOWIE_BUILD_SERVER` — build the standalone MQTT server.
- `FLOWIE_BUILD_TESTS` — build tests.
- `FLOWIE_BUILD_CLUSTER` — enable optional cluster integration.
- `FLOWIE_BUILD_CONTROL` — enable optional control-plane components.

## Required installed SDKs

The current top-level build resolves dependencies explicitly from configured package roots:

- `SALTS_ROOT` — Salts profile
- `SALTS_UTILS_ROOT` — SaltsUtils profile
- `HTTP_SERVICES_ROOT` — current CHTTP install root
- `TURBODB_ROOT` — TurboDB/ORM profile

Cluster builds additionally use the configured FlowMQ and TurboRaft roots.

The build fails when the configured package roots are missing or invalid; it does not silently fall back to unrelated profiles.

The package roots select the SDKs used for a build. `find_package` does not download or update them, so update each installed profile to the latest release before configuring Flowie.

The historical variable name `HTTP_SERVICES_ROOT` currently points at the CHTTP package installation. It is a package-root name, not a separate runtime boundary.

## HTTP and WebSocket integration

HTTP/WebSocket support comes from the standalone [CHTTP](https://github.com/qigao/chttp) SDK:

- outbound HTTP and WebSocket client code links `CHttp::Client`;
- HTTP and WebSocket listeners link `CHttp::Server`;
- TCP/TLS transport remains owned by Salts CNet.

This lets MQTT-over-WebSocket share the same explicit transport, lifecycle, and error semantics as the rest of the Salts ecosystem.

## Parser and utility integration

JSON, YAML, command-line, LTV, and related parser capabilities come from the installed [SaltsUtils](https://github.com/qigao/salts-utils) SDK.

These are independent component dependencies, not evidence that Flowie owns a parser runtime.

## Run the standalone broker

```powershell
flowie_server --host 0.0.0.0 --port 1883 --transport tcp
```

WebSocket listener example:

```powershell
flowie_server --host 0.0.0.0 --port 8080 --transport ws --path /mqtt
```

TLS/WSS certificate configuration uses:

```text
SALTS_TLS_CERT_FILE
SALTS_TLS_KEY_FILE
```

Validate startup configuration without listening:

```powershell
flowie_server --check
```

TCP/TLS brokers can opt into multiple fixed CNet owners with `--network-workers 4`
or `network_workers: 4` in the endpoint adapter's YAML `config`. Zero or one keeps
the single-owner default. `network_command_bytes` (`--network-command-bytes`) is
an aggregate mailbox budget, divided across owners; each partition must fit one
maximum-sized packet. The default 16 MiB budget supports four owners with the
default 1 MiB packet limit. MQTT session/topic state stays on its existing executor
shard. See [network ownership and shutdown](flowie/ARCHITECTURE.md#tcptls-network-owners)
for capacity, ABI, transport, and lifecycle constraints.

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
reject explicit binding. UDP/KCP remain single-owner; WS/WSS also reject these
placement options. No throughput improvement is claimed without measurement.

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

## CMake consumption

```cmake
find_package(Flowie CONFIG REQUIRED)
target_link_libraries(my_broker PRIVATE Flowie::Flowie)
```

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
