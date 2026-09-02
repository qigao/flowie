# Flowie Client MQTT 5 Default Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make MQTT 5 the Flowie Client default while adding an explicit, fail-fast MQTT 3 version-selection API and preserving complete version-specific wire validation.

**Architecture:** Keep a mutex-protected selected protocol version separate from the worker-owned active connection version. Resolve `UNSPECIFIED` packet versions atomically at command admission, reject mismatches before queue ownership transfers, and leave packet-specific MQTT 5/3 validation in the existing protocol encoder.

**Tech Stack:** C11, Flowie MQTT protocol codec, CoroNet worker/command queue, TurboUtils TinyTest, CMake presets.

**Spec:** `docs/superpowers/specs/2026-09-02-flowie-client-protocol-version-policy.md`

## Global Constraints

- Default selected version is exactly `FLOWIE_MQTT_VERSION_5`.
- MQTT 3 means MQTT 3.1.1 by recommendation; MQTT 3.1 remains explicitly selectable.
- Do not change public structure layouts, persisted data, wire formats, dependencies, or deployment configuration.
- Unsupported versions and explicit mismatches fail; no protocol fallback or auto-negotiation is allowed.
- The protocol codec remains the sole validator for packet-specific MQTT 5 properties and MQTT 3 field restrictions.
- Tests follow strict red-green-refactor and run through `win-release-user`.

---

### Task 1: Default and selectable protocol version

**Files:**
- Modify: `flowie/client/include/flowie_mqtt_client.h`
- Modify: `flowie/client/src/flowie_mqtt_client.c`
- Test: `flowie/client/tests/test_flowie_mqtt_client.c`

**Interfaces:**
- Consumes: `flowie_mqtt_version_t`, `flowie_mqtt_version_is_supported()`, `command_mutex`.
- Produces: `int flowie_mqtt_client_set_version(flowie_mqtt_client_t *, flowie_mqtt_version_t)` and `UNSPECIFIED` inheritance for CONNECT/PUBLISH/SUBSCRIBE/UNSUBSCRIBE.

- [ ] **Step 1: Write failing wire-level default and setter tests**

Extend the existing local broker callback harness so packet descriptions can remain
`FLOWIE_MQTT_VERSION_UNSPECIFIED`. Add cases that expect MQTT 5 without a setter and MQTT 3.1.1/3.1
after calling the setter. The broker must parse the actual CONNECT and compare its wire version.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
cmake --build --preset win-release-user --target test_flowie_mqtt_client
build\Msvc-Release\bin\test_flowie_mqtt_client.exe --filter "selected protocol version"
```

Expected: compile failure because `flowie_mqtt_client_set_version` is not declared, proving the new
public contract does not exist.

- [ ] **Step 3: Implement the minimum selected-version state and API**

Add `selected_version` and `version_locked` to the opaque client. Initialize the selection to MQTT 5.
Implement the setter under `command_mutex`, accepting only the three supported concrete versions and
returning `TURBO_EALREADY` after lock-in.

- [ ] **Step 4: Resolve versioned commands during atomic admission**

For CONNECT, PUBLISH, SUBSCRIBE, and UNSUBSCRIBE, replace `UNSPECIFIED` with `selected_version` while
holding `command_mutex`; reject a concrete mismatch with `TURBO_EPROTO`. Lock the policy only after
the command and wake post are both accepted. Apply the same rules atomically to publish batches.

- [ ] **Step 5: Run the focused test and verify GREEN**

Run the filtered executable again. Expected: all selected-version cases pass and each broker observes
the exact requested protocol level.

- [ ] **Step 6: Commit the API behavior**

```powershell
git add flowie/client/include/flowie_mqtt_client.h flowie/client/src/flowie_mqtt_client.c flowie/client/tests/test_flowie_mqtt_client.c
git commit -m "feat(flowie): default MQTT client to version 5"
```

### Task 2: Fail-fast version boundaries and reconnect consistency

**Files:**
- Modify: `flowie/client/src/flowie_mqtt_client.c`
- Test: `flowie/client/tests/test_flowie_mqtt_client.c`

**Interfaces:**
- Consumes: selected-version API and admission resolver from Task 1.
- Produces: stable error semantics for invalid selection, version mismatch, lock-in, and refreshed CONNECT packets.

- [ ] **Step 1: Write failing boundary tests**

Add independent TinyTest cases for null/unsupported/unspecified selection, selection after a CONNECT is
accepted, explicit packet mismatch, and a refresh callback that attempts to change the protocol.
Assert literal `TURBO_EINVAL`, `TURBO_EALREADY`, or `TURBO_EPROTO` results and no version fallback.

- [ ] **Step 2: Run the boundary filter and verify RED**

Run:

```powershell
build\Msvc-Release\bin\test_flowie_mqtt_client.exe --filter "protocol version boundary"
```

Expected: at least the refreshed CONNECT mismatch case fails because reconnect replacement does not
yet enforce the selected protocol.

- [ ] **Step 3: Enforce the same resolver for refreshed CONNECT packets**

Resolve `UNSPECIFIED` to the immutable selected version and reject a different concrete version before
replacing retained credentials. Preserve credential wiping and the first useful error.

- [ ] **Step 4: Run boundary and complete client tests**

Run:

```powershell
build\Msvc-Release\bin\test_flowie_mqtt_client.exe --filter "protocol version"
ctest --preset win-release-user -R ^test_flowie_mqtt_client$ --output-on-failure
```

Expected: all version boundaries and the full client suite pass.

- [ ] **Step 5: Commit boundary enforcement**

```powershell
git add flowie/client/src/flowie_mqtt_client.c flowie/client/tests/test_flowie_mqtt_client.c
git commit -m "test(flowie): enforce MQTT client version boundaries"
```

### Task 3: Consumer migration and documentation

**Files:**
- Modify: `flowie/CLIENT_GUIDE.md`
- Modify: `flowie/src/flowie_client_adapter.c`
- Modify: `flowie/tests/test_flowie_transport.c`
- Modify: `flowie/tests/test_flowie_transport_baseline.c`
- Modify: other in-repository Flowie Client consumers identified by `rg.exe` if they explicitly select MQTT 3.
- Test: `flowie/tests/test_flowie_client_adapter.c`

**Interfaces:**
- Consumes: `flowie_mqtt_client_set_version()`.
- Produces: documented default-v5 usage and explicit MQTT 3 migration examples.

- [ ] **Step 1: Add an MQTT 3 consumer regression where applicable**

Before changing consumers, add or adjust a real adapter/transport test so a configured MQTT 3 path
calls the setter and succeeds with `UNSPECIFIED` packet versions.

- [ ] **Step 2: Run the closest consumer test and verify RED**

Build and run the exact affected target through `win-release-user`; expect the MQTT 3 path to fail
until the consumer selects its version.

- [ ] **Step 3: Migrate consumers and examples**

Remove redundant MQTT 5 packet assignments where doing so demonstrates the default. Add an explicit
setter call before MQTT 3 commands. Keep the Flowie TurboFlow adapter intentionally on default MQTT 5.

- [ ] **Step 4: Update the client guide**

Document the default, setter timing, exact error conditions, `UNSPECIFIED` inheritance, mismatch
behavior, MQTT 3.1.1 recommendation, and the existing MQTT 5 properties/reason-code/enhanced-auth
support. Include compilable MQTT 5 and MQTT 3.1.1 snippets.

- [ ] **Step 5: Build and run adjacent tests**

Run the client target, transport tests, adapter tests, and protocol tests selected by CodeGraph impact.
Expected: all pass with zero failures.

- [ ] **Step 6: Commit migration and docs**

```powershell
git add flowie/CLIENT_GUIDE.md flowie/src/flowie_client_adapter.c flowie/tests
git commit -m "docs(flowie): document MQTT client version policy"
```

### Task 4: Final verification

**Files:**
- Verify only: all changed files and generated build outputs.

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: reproducible build, test, diff, and compatibility evidence.

- [ ] **Step 1: Synchronize CodeGraph and inspect affected tests**

Run `codegraph sync .` and `codegraph affected -p . <changed-files>`; confirm all direct consumers are
covered by the selected tests.

- [ ] **Step 2: Run release build and test gates**

Run the focused client suite first, then all Flowie release tests warranted by the affected graph.
Record any environment-only DLL repair separately from source results.

- [ ] **Step 3: Inspect source and exported API diffs**

Run `git diff --check`, `git status --short`, and inspect the complete diff. Confirm `.codegraph/` and
build artifacts are not tracked, no public struct layout changed, and no placeholders were introduced.

- [ ] **Step 4: Commit final corrections, if any**

Commit only reviewed source, tests, specs, plans, and documentation; do not commit local index or build
artifacts.

