# Flowie Scale Load Validation Implementation Plan

> **For Codex:** Execute this checklist in the current workspace because the task continues an existing dirty-worktree remote-debug session; do not create an isolated worktree that would omit those changes.

**Goal:** Run repeatable 96/192/384-client MQTT fan-out loads against the native Debug Flowie server and fail on content, ordering, process, health, resource, or log violations.

**Architecture:** A repository-owned Bash runner starts subscribers before publishers, uses a unique namespace per tier, validates every subscriber against a deterministic oracle, and samples the already-running server through `/proc` and `ss`. A separate loopback listener isolates scale testing from the existing diagnostic listener and protected PostgreSQL/Nginx containers.

**Tech Stack:** Bash, Mosquitto CLI, GNU coreutils/awk/sort, Linux `/proc`, `ss`, native `flowie_server`, tlog DEBUG output.

---

### Task 1: Define bounded load and validation contracts

**Files:**
- Create: `deploy/server/tests/run-mqtt-scale-load.sh`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

- [x] Define tiers as subscribers/publishers per QoS = 16/32/64, producing 96/192/384 simultaneous clients.
- [x] Check connection and per-session inflight capacity before launch.
- [x] Use unique run/client/topic identifiers and deterministic payload fields.
- [x] Require subscriber readiness before starting publishers.

### Task 2: Implement correctness and lifecycle oracles

**Files:**
- Create: `deploy/server/tests/run-mqtt-scale-load.sh`

- [x] Require every Mosquitto process to exit successfully within a bounded timeout.
- [x] Validate exact counts plus sorted multiset equality for missing, duplicate, foreign, or damaged messages.
- [x] Validate per-publisher FIFO sequence independently for every subscriber and QoS.
- [x] Verify listener/PID health after each tier and clean up only runner-owned child processes.

### Task 3: Implement resource and log evidence

**Files:**
- Create: `deploy/server/tests/run-mqtt-scale-load.sh`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

- [x] Sample server RSS, virtual memory, thread count, FD count, and established connections.
- [x] Record elapsed time, delivery count, throughput, and before/peak/settled resource values per tier.
- [x] Inspect only each tier's log delta for WARN/ERROR/FATAL, malformed component/source, sensitive field names, and bounded QoS2 DEBUG volume.
- [x] Save machine-readable summaries and retain failure artifacts.

### Task 4: Verify locally and on EU

**Files:**
- Test: `deploy/server/tests/run-mqtt-scale-load.sh`
- Test: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

- [x] Run `bash -n` and the runner's argument/capacity negative checks locally or remotely.
- [x] Start an isolated native Debug listener on EU with explicit scale limits.
- [x] Run tiers 16, 32, and 64; preserve per-tier artifacts and summary.
- [x] Confirm the existing port 18889 server and protected PostgreSQL/Nginx containers remain healthy.
