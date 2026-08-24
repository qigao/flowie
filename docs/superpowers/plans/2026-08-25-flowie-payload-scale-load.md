# Flowie Payload-Aware Scale Load Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the existing Flowie remote MQTT scale runner with exact 100--4096 byte payloads, byte-volume evidence, and a bounded client/payload load matrix.

**Architecture:** Keep the runner's deterministic metadata as the canonical payload prefix and append deterministic ASCII `x` padding to reach an exact requested byte count. Preserve the existing unthrottled publisher barrier, per-publisher FIFO validation, multiset validation, process checks, and log gates; add payload-byte capacity calculations and CSV evidence without changing the default legacy payload behavior.

**Tech Stack:** Bash, Mosquitto CLI, GNU awk/coreutils, Flowie native Debug server, Linux `/proc` resource sampling.

**Spec:** `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`, section 4.2, plus the user requirement “more clients, data size from 100bytes to 4kbytes”.

## Global Constraints

- Preserve existing behavior when `--payload-bytes` is omitted.
- Accept configured payload sizes only in the inclusive range 100--4096 bytes and fail fast outside it.
- Keep payloads single-line ASCII so `mosquitto_pub -l`, subscriber line counts, multiset comparison, and FIFO validation retain their current semantics.
- For configured payloads, require every generated and received line to be exactly the requested byte count and require all padding bytes to be `x`.
- Continue to bind only `127.0.0.1`, replace no server process, and leave PostgreSQL/Nginx untouched.
- Capacity facts use `connections = 6N`, `inflight_per_session = N * messages`, `deliveries = 3 * N * N * messages`, and `delivery_payload_bytes = deliveries * payload_bytes` with signed-64-bit overflow rejection.
- Inputs are copied into run-scoped files, read by one publisher process, copied/owned by the MQTT/Flowie delivery path, and released when clients and sessions drain. Full/timeout behavior remains explicit through current runner failures.

---

### Task 1: Specify exact payload behavior with a failing shell regression

**Files:**
- Modify: `deploy/server/tests/test-mqtt-scale-runner.sh`
- Test: `deploy/server/tests/test-mqtt-scale-runner.sh`

**Interfaces:**
- Consumes: `run-mqtt-scale-load.sh --plan`.
- Produces: regression expectations for `--payload-bytes 100`, `--payload-bytes 4096`, and rejection of 99/4097.

- [x] **Step 1: Add plan-mode assertions before the existing detached-log test**

Run the real runner with `--plan --tiers 1 --messages 1 --payload-bytes 100` and 4096. Require output fields `payload_bytes=100` / `4096` and exact `expected_delivery_payload_bytes=300` / `12288`. Run 99 and 4097 and require non-zero exit plus the inclusive-range diagnostic.

- [x] **Step 2: Run the shell regression and verify RED**

Run:

```bash
bash deploy/server/tests/test-mqtt-scale-runner.sh
```

Expected: FAIL because the current runner rejects the unknown `--payload-bytes` option.

### Task 2: Implement bounded exact payload generation and byte evidence

**Files:**
- Modify: `deploy/server/tests/run-mqtt-scale-load.sh`
- Modify: `deploy/server/tests/test-mqtt-scale-runner.sh`

**Interfaces:**
- Consumes: optional CLI `--payload-bytes N`.
- Produces: `build_payload RUN QOS PUBLISHER SEQUENCE`, plan fields `payload_bytes` and `expected_delivery_payload_bytes`, and appended CSV fields `payload_bytes`, `expected_delivery_payload_bytes`, and `delivery_payload_bytes_per_second`.

- [x] **Step 1: Parse and validate the optional payload size**

Keep the default empty value for legacy metadata-only payloads. When configured, require a positive integer in `[100, 4096]`; include the value in usage and capacity-plan output.

- [x] **Step 2: Add deterministic exact-size payload generation**

Build the existing five-field metadata prefix, reject a target shorter than that prefix, append only ASCII `x`, and verify `${#payload}` equals the target before writing it. The same helper must be used by plan-mode self-check and live input generation.

- [x] **Step 3: Extend content validation and summary evidence**

Pass the expected byte count to `validate_payload_sequence`. Preserve exact marker matching for legacy payloads; for configured payloads require the marker prefix, an all-`x` suffix, and exact `length($0)`. Append byte count, expected total delivered payload bytes, and delivered payload bytes/second to the existing CSV schema so earlier columns retain their positions.

- [x] **Step 4: Run the focused regression to GREEN**

Run the shell regression and require both exact-size plan cases, both range failures, and the existing detached-server-log contract to pass.

### Task 3: Document and execute the remote matrix

**Files:**
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`
- Upload: `deploy/server/tests/run-mqtt-scale-load.sh`
- Upload: `deploy/server/tests/test-mqtt-scale-runner.sh`

**Interfaces:**
- Consumes: the existing native Debug server on `127.0.0.1:18890`, `max_connections=2048`, `max_inflight=8192`.
- Produces: run-scoped summaries for payload and client scaling.

- [x] **Step 1: Update the runbook contract and examples**

Document exact configured payload semantics, the new CSV byte fields, and formulas. Preserve the statement that publisher rate is unthrottled.

- [x] **Step 2: Upload and run the shell regression remotely**

Run the test script from the current `/root/dev/runs/20260824T104051Z-flowie-log-p0` source tree before load execution.

- [x] **Step 3: Execute the payload sweep**

At `N=32`, `messages=25`, run exact payload sizes 100, 256, 1024, and 4096 bytes. Each tier has 192 clients and 76,800 deliveries; expected delivered payload ranges from 7,680,000 to 314,572,800 bytes.

The 4096-byte tier first demonstrated the configured 1 MiB send HWM boundary through slow-subscriber
isolation. It passed after restarting only the Flowie listener with a calculated 4 MiB per-connection HWM;
the failed artifact was retained as capacity evidence.

- [x] **Step 4: Execute the client sweep**

At 100 bytes and `messages=25`, run `N=64,96,128`, corresponding to 384, 576, and 768 clients and 307,200, 691,200, and 1,228,800 deliveries. Stop escalation if host memory, process, connection, log, or correctness gates fail.

- [x] **Step 5: Execute the combined bounded stress case**

Run `N=96`, `messages=5`, `payload_bytes=4096`: 576 clients, 138,240 deliveries, and 566,231,040 delivered payload bytes. This keeps per-session inflight at 480 and avoids the approximately 10.1 GB fan-out payload implied by `N=128`, `messages=50`, `payload_bytes=4096`.

- [x] **Step 6: Review resource settling and protected services**

Compare elapsed time, CPU, throughput, payload bytes/second, RSS/VM/thread/fd/connection peaks, settled RSS/fds, and log deltas. Require the listener to remain present and PostgreSQL/Nginx to remain healthy.
