# Flowie Delivery Index Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Flowie's quadratic outbound-delivery packet-id scans with a bounded derived index while preserving MQTT delivery, persistence, and ownership behavior.

**Architecture:** The existing `deliveries` vector remains the canonical owner of delivery state and packet memory. A session-local TurboSTL hash map derives `packet_id -> vector slot`; centralized append/remove helpers update both structures transactionally, and clone/restore rebuild the index from the canonical vector data.

**Tech Stack:** C11, Flowie session owner, TurboSTL `vec_t` and `hash_map_t`, TinyTest, CMake presets, remote Linux MQTT scale runner.

**Spec:** `docs/superpowers/plans/2026-08-24-flowie-scale-load-validation.md`

## Global Constraints

- Preserve all public APIs, MQTT wire behavior, persistence formats, and existing remote-runner semantics.
- Keep `deliveries` as the sole canonical state; the hash map is a derived index and never owns packets.
- Bound the index to `flowie_session_config_t.max_inflight`, which is already validated as `1..UINT16_MAX`.
- The session owner is single-owner mutable state; this change adds no locks and no cross-thread access.
- Index maintenance failures return existing Turbo error codes and never fall back to an O(D) scan.
- Preserve the existing dirty-worktree changes and do not modify protected PostgreSQL or Nginx services.

---

### Task 1: Add a high-cardinality session regression

**Files:**
- Create: `flowie/tests/test_flowie_session_delivery_index.c`
- Modify: `flowie/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `flowie_session_owner_create`, `flowie_session_owner_open`, `flowie_session_owner_delivery_reserve`, `flowie_session_owner_delivery_cancel`, `flowie_session_owner_clone`, and `flowie_session_owner_repository_restore` from `flowie_session_internal.h`.
- Produces: CTest target `test_flowie_session_delivery_index` covering high-cardinality reserve latency, swap-remove lookup consistency, clone rebuild, and repository restore rebuild.

- [x] **Step 1: Write the failing performance regression**

Create a TinyTest case that reserves all 65,535 legal QoS1 packet identifiers in one active session, checks the final inflight count, and requires the reserve loop to complete within 3,000 ms. Record elapsed time with `turbo_monotonic_ms()` and keep assertions outside the hot loop.

- [x] **Step 2: Add correctness cases for the derived-index boundaries**

Add cases that remove middle entries and then address swap-moved entries by packet id, repeat the sequence on a clone, and restore multiple deliveries from `flowie_protocol_session_row_t` before removing them out of vector order.

- [x] **Step 3: Register and build the focused test**

Run:

```powershell
cmd /c "call \"<VsDevCmd.bat>\" -arch=x64 -host_arch=x64 >nul && cmake --fresh --preset win-release-user && cmake --build --preset win-release-user --target test_flowie_session_delivery_index"
```

Expected: the target builds; the high-cardinality test fails on the current linear scan by exceeding 3,000 ms, while the correctness cases pass.

### Task 2: Add the canonical-vector derived delivery index

**Files:**
- Modify: `flowie/src/flowie_session.c`
- Test: `flowie/tests/test_flowie_session_delivery_index.c`

**Interfaces:**
- Consumes: `hash_map_init_bytes`, `hash_map_get`, `hash_map_put`, `hash_map_remove`, `hash_map_clear`, `hash_map_destroy`, `vec_push`, `vec_pop`, and `vec_swap_remove`.
- Produces: private helpers `flowie_session_delivery_lookup`, `flowie_session_delivery_append`, and `flowie_session_delivery_remove_at`; no public ABI change.

- [x] **Step 1: Initialize and destroy the bounded index**

Add `hash_map_t delivery_index` to `flowie_session_owner_s`, initialize it with `uint16_t` keys, `size_t` values, and `config.max_inflight` entry limit after the deliveries vector initializes, and unwind all previously initialized containers on failure. Clear it with delivery state and destroy it during owner teardown.

- [x] **Step 2: Centralize indexed lookup and append**

Implement average O(1) lookup that validates the mapped slot and packet id. Implement append as vector push followed by map insertion; if map insertion fails, pop the just-added vector element so the canonical state remains unchanged.

- [x] **Step 3: Centralize swap-removal maintenance**

Implement removal by erasing the removed packet id, calling `vec_swap_remove`, and updating the already-existing index value for the element moved into the removed slot. Return `TURBO_EPROTO` when a derived-index invariant is missing or contradictory.

- [x] **Step 4: Route every delivery mutation through the helpers**

Use indexed lookup for reserve/commit/ACK/packet-expiry and record duplicate checks. Use indexed append for reserve, clone, repository restore, and record restore. Use indexed removal for cancel, bulk expiry, packet expiry, and completed ACK handling.

- [x] **Step 5: Run the focused test to green**

Run the target directly with `--filter` for the high-cardinality case, then run the entire test executable. Expected: all cases pass and the reserve loop is below 3,000 ms.

### Task 3: Verify local and remote behavior

**Files:**
- Test: `flowie/src/flowie_session.c`
- Test: `flowie/tests/test_flowie_session_delivery_index.c`
- Test: `deploy/server/tests/run-mqtt-scale-load.sh`

**Interfaces:**
- Consumes: Windows `win-release-user` and `win-dev-user` presets plus the existing `/root/dev` native-server remote runner workflow.
- Produces: local functional/ASan evidence and remote elapsed/CPU/throughput evidence for the previously slow client/message tiers.

- [x] **Step 1: Run local adjacent regression tests**

Run the focused CTest target and the Flowie release-labeled test set. Run the focused target under `win-dev-user` to cover AddressSanitizer cleanup and clone/restore ownership.

Release verification passed. The Windows ASan configure is environment-blocked because the configured
`C:/projects/cpp/external/pkgs/turbodb/debug` SDK directory is absent; the remote Debug build and end-to-end
load checks cover Linux compilation and runtime behavior, but do not replace that outstanding ASan gate.

- [x] **Step 2: Upload the changed source and rebuild the native Linux server**

Use the runbook's `/root/dev` source and artifact layout. Stop only the Flowie server/listener being replaced; do not stop or remove PostgreSQL/Nginx containers.

- [x] **Step 3: Repeat the diagnosed load tiers**

Run the remote runner for the prior 96-client message counts, including 25 and 50 messages per publisher, with exact content/count/order/process/log validation enabled. Compare elapsed time, CPU time, throughput, resource high-water marks, and WARN/ERROR/FATAL deltas to the preserved baseline artifacts.

- [x] **Step 4: Review the final diff and record evidence**

Confirm only planned files changed, no persistence/public API format changed, no debug-only logs were added to the hot path, and all focused/local/remote checks have reproducible artifact paths.
