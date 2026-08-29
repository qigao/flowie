# Flowie Single-Container Deployment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the standard Compose deployment run Flowie Broker and Control in one `flowie-server` container with coupled fail-fast lifecycle and dual-listener health checks.

**Architecture:** Add a small POSIX-shell supervisor that delegates argument construction and secret loading to the two existing entrypoints. Compose explicitly selects the supervisor while the image default remains Broker-only; separate nested data volumes preserve existing state ownership.

**Tech Stack:** POSIX shell, Dockerfile, Docker Compose, Docker healthcheck, existing Flowie binaries.

**Spec:** `docs/superpowers/specs/2026-08-25-flowie-single-container.md`

## Global Constraints

- Keep MQTT, HTTPS, authentication, ACL, TLS and SQLite formats unchanged.
- Keep direct image execution Broker-only unless Compose selects the combined entrypoint.
- Run as UID/GID `10001:10001` with the existing read-only and capability restrictions.
- Fail fast when either child exits and forward TERM/INT/HUP to both children.
- Preserve `flowie-data` and `flowie-control-data` as separate facts and backup boundaries.

---

### Task 1: Add Server check-only entrypoint behavior

**Files:**
- Modify: `deploy/server/tests/test-docker-entrypoint.sh`
- Modify: `deploy/server/docker-entrypoint.sh`

**Interfaces:**
- Consumes: existing `FLOWIE_*` environment-to-CLI mapping.
- Produces: `FLOWIE_CHECK` boolean, which appends `--check` to the generated `flowie_server` command.

- [x] **Step 1: Write the failing test**

Add a second invocation with `FLOWIE_CHECK=yes` and assert the fake executable receives a leading literal `--check` followed by the existing generated arguments. Add an invalid `FLOWIE_CHECK=maybe` case that must exit before invoking the fake server.

- [x] **Step 2: Run test to verify it fails**

Run: `sh deploy/server/tests/test-docker-entrypoint.sh`

Expected: FAIL because `--check` is absent or invalid `FLOWIE_CHECK` is accepted.

- [x] **Step 3: Write minimal implementation**

Default `FLOWIE_CHECK` to `0`, validate it with `flowie_validate_bool`, and prepend `--check` only for `1|true|yes|on`:

```sh
set -- flowie_server
case "$FLOWIE_CHECK" in
  1|true|yes|on) set -- "$@" --check ;;
esac
set -- "$@" --host "$FLOWIE_HOST"
```

- [x] **Step 4: Run test to verify it passes**

Run: `sh deploy/server/tests/test-docker-entrypoint.sh`

Expected: `docker-entrypoint contract: PASS`.

### Task 2: Add combined lifecycle supervisor

**Files:**
- Create: `deploy/server/tests/test-flowie-combined-entrypoint.sh`
- Create: `deploy/server/flowie-combined-entrypoint.sh`
- Modify: `deploy/server/Dockerfile`

**Interfaces:**
- Consumes: `/usr/local/bin/docker-entrypoint.sh`, `/usr/local/bin/flowie-control-entrypoint`, `FLOWIE_COMBINED_CHECK`.
- Produces: one PID-supervised container lifecycle for the two existing executables.

- [x] **Step 1: Write the failing test**

Use real temporary fake entrypoints, not mocked assertions. Cover these observable contracts:

```text
check mode -> both child checks run sequentially and no long-running child remains
control exits 23 -> server receives TERM and combined entrypoint exits 23
combined receives TERM -> both children receive TERM and combined entrypoint exits 143
explicit command -> command is exec'd without starting either service
```

- [x] **Step 2: Run test to verify it fails**

Run: `sh deploy/server/tests/test-flowie-combined-entrypoint.sh`

Expected: FAIL because `flowie-combined-entrypoint.sh` does not exist.

- [x] **Step 3: Write minimal implementation**

Validate `FLOWIE_COMBINED_CHECK`. In check mode invoke both existing entrypoints with their check flags. In normal mode launch both as direct background children, install TERM/INT/HUP traps, poll until either exits, terminate the sibling, `wait` for both, and convert an unexpected zero child exit to `1`.

- [x] **Step 4: Run test to verify it passes**

Run: `sh deploy/server/tests/test-flowie-combined-entrypoint.sh`

Expected: `flowie-combined-entrypoint contract: PASS`.

- [x] **Step 5: Package the entrypoint**

Add an executable Dockerfile copy:

```dockerfile
COPY --chmod=0755 deploy/server/flowie-combined-entrypoint.sh \
  /usr/local/bin/flowie-combined-entrypoint
```

### Task 3: Make healthcheck cover both listeners

**Files:**
- Create: `deploy/server/tests/test-healthcheck.sh`
- Modify: `deploy/server/healthcheck.sh`

**Interfaces:**
- Consumes: existing `FLOWIE_HEALTH_HOST/PORT` and optional `FLOWIE_HEALTH_SECONDARY_HOST/PORT`.
- Produces: success only when PID 1 and every configured listener are reachable.

- [x] **Step 1: Write the failing test**

Run the real healthcheck with a fake `nc` executable that records calls and can fail a selected port. Assert one-port compatibility, two literal host/port probes, rejection of invalid secondary ports, and failure when the secondary probe fails.

- [x] **Step 2: Run test to verify it fails**

Run: `sh deploy/server/tests/test-healthcheck.sh`

Expected: FAIL because the secondary listener is not probed.

- [x] **Step 3: Write minimal implementation**

Extract a local numeric/range validation function, always probe the primary listener, and probe the secondary only when `FLOWIE_HEALTH_SECONDARY_PORT` is non-empty.

- [x] **Step 4: Run test to verify it passes**

Run: `sh deploy/server/tests/test-healthcheck.sh`

Expected: `flowie healthcheck contract: PASS`.

### Task 4: Collapse Compose to one service and update operations docs

**Files:**
- Modify: `deploy/server/compose.yml`
- Modify: `deploy/server/.env.example`
- Modify: `deploy/server/README.md`

**Interfaces:**
- Consumes: combined entrypoint and dual-listener healthcheck from Tasks 2-3.
- Produces: one `flowie-server` Compose service and one container with separate Broker/Control volumes.

- [x] **Step 1: Update Compose**

Set the service entrypoint to `/usr/local/bin/flowie-combined-entrypoint`, move Control environment, secret, config and cert mounts into `flowie-server`, and mount:

```yaml
volumes:
  - flowie-data:/var/lib/flowie
  - flowie-control-data:/var/lib/flowie/control
```

Set the secondary health variables from `FLOWIE_CONTROL_HEALTH_HOST/PORT` and remove the `flowie-control` service.

- [x] **Step 2: Update examples and docs**

Document the coupled lifecycle, one-service commands, check-only command using `FLOWIE_COMBINED_CHECK=1`, nested volume ownership, upgrade downtime, backup and rollback behavior.

- [x] **Step 3: Run all shell contracts**

Run:

```sh
sh deploy/server/tests/test-docker-entrypoint.sh
sh deploy/server/tests/test-flowie-control-entrypoint.sh
sh deploy/server/tests/test-flowie-combined-entrypoint.sh
sh deploy/server/tests/test-healthcheck.sh
```

Expected: all four scripts print `PASS` and exit `0`.

### Task 5: Build, migrate and verify on EU

**Files:**
- Runtime-only deployment artifacts outside the repository.

**Interfaces:**
- Consumes: the source tree from Tasks 1-4 and the current EU Control SQLite/cert/config mounts.
- Produces: one healthy EU `flowie-server` container running both listeners.

- [x] **Step 1: Validate source and Compose**

Run shell syntax checks, all entrypoint contracts, `docker compose config`, and `FLOWIE_COMBINED_CHECK=1` against the built image.

- [x] **Step 2: Build immutable test image**

Use the documented seven named build contexts, record the source revision/content marker and image digest, and require both final `ldd` manifests to contain no `not found`.

- [x] **Step 3: Preserve rollback state and migrate**

Record current container inspect output, stop both services, retain the old immutable images and volumes, then create one `flowie-server` container using the existing Server data volume and existing Control config/certs/SQLite bind data.

- [x] **Step 4: Verify runtime behavior**

Require exactly one running Flowie container, both ports on loopback, healthy status and zero automatic restarts. Execute MQTT CONNECT/PING/DISCONNECT plus CA-verified HTTPS bootstrap login, dashboard-to-password redirect and logout.

- [x] **Step 5: Verify restart persistence and rollback readiness**

Restart the combined container, wait for health, repeat both protocol tests, and confirm the same SQLite file remains mounted. If any gate fails, restore the previous two containers from recorded images and mounts.
