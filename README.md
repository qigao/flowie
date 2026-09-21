# Flowie

**MQTT server/client infrastructure built on the Salts ecosystem.**

Flowie owns MQTT protocol behavior, broker core state, sessions, subscriptions, retained messages, authentication boundaries, and the server/client lifecycle. It reuses Salts for networking/runtime semantics and CHTTP for HTTP/WebSocket transport integration instead of introducing a second systems runtime.

**Tags:** C11 · C++17 · MQTT · broker · pub-sub · WebSocket · TLS · networking · Salts · CHTTP

## Built on Salts

Flowie is intentionally layered on the shared Salts foundation:

- **Salts::CNet** provides TCP/TLS transport and explicit connection/session progress.
- **Salts Core / Coroutine / runtime primitives** provide common systems facilities.
- **SaltsUtils** provides JSON/YAML/command-line/LTV and related parser components used by broker/server features.
- **CHTTP** provides HTTP and WebSocket client/server infrastructure.
- **TurboDB ORM** provides database integration through the configured installed profile.
- optional cluster mode integrates with FlowMQ and TurboRaft through their public packages.

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
