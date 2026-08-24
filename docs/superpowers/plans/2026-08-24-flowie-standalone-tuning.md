# Flowie Standalone Server Tuning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the existing Flowie endpoint capacity, backpressure, buffer, timeout, keepalive, and listener-reuse controls through the standalone server CLI and Docker deployment contract.

**Architecture:** `flowie_server` remains a restart-only composition root. It parses bounded scalar options, validates casts and cross-field constraints, copies them into the existing `flowie_endpoint_config_t`, and records a bounded three-record DEBUG effective-configuration group before endpoint creation. The Docker entrypoint maps environment variables to exactly those CLI options; no new queue, worker runtime, hot reload, or endpoint API is introduced.

**Tech Stack:** C11, TurboUtils command parser and tlog, Flowie endpoint API, CMake/CTest, POSIX shell, Docker Compose.

**Spec:** `docs/superpowers/plans/2026-08-24-flowie-standalone-tuning.md`

## Global Constraints

- Preserve all current standalone defaults and MQTT behavior.
- Configuration is startup-only; changing any value requires a server restart.
- `flowie_endpoint_config_t` remains the single runtime configuration fact source.
- Zero-valued coroutine, stream, socket-buffer, and timeout fields preserve their current component defaults.
- Capacity and range errors fail before the listener starts; no silent clamping or fallback is allowed.
- `send_hwm_bytes` retains the existing per-connection disconnect behavior.
- The effective-config group is DEBUG-only, emitted once as three bounded records, and contains no credentials, client IDs, topics, or payloads.
- Worker count remains a YAML supervisor/runtime concern and is not added to the direct standalone broker.

---

### Task 1: Standalone CLI capacity and I/O contract

**Files:**
- Modify: `server/CMakeLists.txt`
- Modify: `server/flowie_server.c`
- Create: `server/cmake/VerifyFlowieServerTuning.cmake`

**Interfaces:**
- Consumes: existing `flowie_endpoint_config_t` fields and Flowie/CoroNet range constants.
- Produces: CLI options `--max-packet-size`, `--max-sessions`, `--max-subscriptions-per-session`, `--max-retained-messages`, `--send-hwm-bytes`, `--coroutine-stack-size`, `--stream-recv-buffer-bytes`, `--socket-recv-buffer-bytes`, `--socket-send-buffer-bytes`, `--timeout-ms`, `--recv-timeout-ms`, `--tcp-keepalive`, `--tcp-keepalive-idle-ms`, `--tcp-keepalive-interval-ms`, `--tcp-keepalive-count`, and `--reuse-port`.

- [x] **Step 1: Add failing CTest product-contract cases**

Add one `--check --log-level DEBUG` case using literal non-default values and assert the effective-config event contains them. Add rejection cases for a 65535-byte coroutine stack and keepalive timing without `--tcp-keepalive`.

- [x] **Step 2: Configure/build and verify the new tests fail**

Run the smallest documented Windows preset target and CTest filter. Expected: the custom options are rejected as unknown or absent from effective configuration.

- [x] **Step 3: Implement minimal CLI parsing, validation, mapping, and logging**

Use signed parser storage, validate before every narrowing cast, map directly to the existing endpoint config, and preserve zero/default semantics. Keep one structured DEBUG effective-config event.

- [x] **Step 4: Rebuild and verify focused tests pass**

Run the same target and CTest filter. Expected: custom configuration passes; undersized stack and orphaned keepalive timing fail.

### Task 2: Docker environment contract

**Files:**
- Modify: `deploy/server/tests/test-docker-entrypoint.sh`
- Modify: `deploy/server/docker-entrypoint.sh`
- Modify: `deploy/server/compose.yml`

**Interfaces:**
- Consumes: Task 1 CLI option names.
- Produces: matching `FLOWIE_*` environment variables with existing component defaults and strict boolean parsing for keepalive/reuse-port.

- [x] **Step 1: Extend the entrypoint contract test first**

Supply literal non-default values for every new environment variable and require the fake `flowie_server` to receive the exact ordered CLI arguments, including enabled boolean flags. Add a case requiring an invalid boolean value to fail.

- [x] **Step 2: Run the shell test remotely and verify RED**

Run `sh deploy/server/tests/test-docker-entrypoint.sh` on the EU Linux staging tree. Expected: the expected argument comparison fails because mappings are absent.

- [x] **Step 3: Implement the entrypoint and Compose mappings**

Add scalar defaults and exact `0|1|false|true|no|yes|off|on` boolean handling. Append flags only when enabled; reject every other boolean spelling before `exec`.

- [x] **Step 4: Run the shell contract and Compose expansion**

Require the shell test and `docker compose config` to succeed without starting, restarting, or removing any container.

### Task 3: Documentation and remote validation

**Files:**
- Modify: `deploy/server/README.md`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

**Interfaces:**
- Consumes: Tasks 1-2 CLI/environment contracts.
- Produces: operator-facing restart-only tuning guidance and a reproducible remote validation command.

- [x] **Step 1: Document every option and resource interaction**

Document defaults, valid ranges, the `max_connections`/session capacity distinction, send-HWM behavior, the two CoroNet receive chunks, OS socket-buffer semantics, keepalive dependencies, restart requirement, and the absence of direct worker-count tuning.

- [x] **Step 2: Build and test the Linux Debug server remotely**

Sync only the scoped files into `/root/dev`, rebuild with the existing native Debug dependency set, and run focused CTest plus `flowie_server --check` with non-default tuning values.

- [x] **Step 3: Run the repository-owned remote load runner**

Start a run-scoped tuned listener, run `deploy/server/tests/run-mqtt-scale-load.sh` at 96/192/384 clients for QoS 0/1/2, and require delivery/content/FIFO/resource/log/process/listener checks to pass.

- [x] **Step 4: Perform the completion audit**

Run `git diff --check`, focused local/remote tests, shell syntax checks, active-reference searches, and verify protected PostgreSQL/Nginx containers remain healthy. Report Debug load results only as host/build/workload evidence, not as a portable performance SLA.
