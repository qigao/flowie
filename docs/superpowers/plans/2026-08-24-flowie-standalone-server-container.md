# Flowie Standalone Server Container Fix Plan

> Execute this plan in the current session. Preserve the independent Flowie broker boundary; do not reconnect the legacy TurboFlow application/supervisor sources.

**Goal:** Make the Flowie server image's default entrypoint match the standalone `flowie_server` CLI and prove the default container works on the EU host.

**Architecture:** `flowie_server` is the product process and accepts listener options directly. The container entrypoint maps environment variables to those supported CLI options. Compose supplies the same environment contract and no longer mounts configuration, graph, Control, certificate, or secret files that the standalone broker does not consume.

**Tech Stack:** POSIX shell, Docker/Compose, CMake-built Flowie server, Mosquitto MQTT 5 clients.

---

### Task 1: Lock the entrypoint contract with a regression test

**Files:**
- Create: `deploy/server/tests/test-docker-entrypoint.sh`

1. Use a fake `flowie_server` to capture the entrypoint's generated argument vector.
2. Assert host, port, transport, WebSocket path, and maximum connections are forwarded exactly.
3. Run the test against the current entrypoint and record the expected failure caused by the stale configuration-file contract.

### Task 2: Align deployment assets with the standalone broker

**Files:**
- Modify: `deploy/server/docker-entrypoint.sh`
- Modify: `deploy/server/Dockerfile`
- Modify: `deploy/server/compose.yml`
- Modify: `deploy/server/README.md`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

1. Replace the legacy security/profile/control/config/graph arguments with supported listener arguments.
2. Replace legacy image environment defaults and description.
3. Remove obsolete Compose secret and configuration mounts; expose the standalone listener variables.
4. Document the new contract and add a default-container runtime test to the runbook.
5. Re-run the entrypoint regression test and shell syntax checks.

### Task 3: Rebuild and verify on the EU Flowie host

**Files:**
- Evidence: `/root/dev/runs/<run-id>/artifacts/flowie-server-runtime-test/`

1. Package the eight current worktrees and build a new immutable Flowie server image under `/root/dev`.
2. Start the image through its default entrypoint on an unused host port.
3. Wait for Docker health, then complete MQTT 5 QoS 1 publish/subscribe through the Flowie broker.
4. Send SIGTERM and assert exit code zero.
5. Remove only run-scoped Flowie test resources and report PostgreSQL/Nginx state unchanged.
