# Flowie Control Container Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Package `flowie-control` in the existing Flowie runtime image and run it as an isolated, persistent, non-root Compose service.

**Architecture:** Reuse `deploy/server/Dockerfile` so one immutable runtime image carries both Flowie executables and their shared libraries, while Compose keeps MQTT and Control as separate services. A dedicated Control entrypoint validates its configuration contract before launching; configuration and certificates are read-only mounts, while the selected Control Repository owns a separate writable volume.

**Tech Stack:** POSIX shell, Docker BuildKit multi-stage builds, Docker Compose, Flowie Control YAML, SQLite, Tiny shell contract tests.

**Spec:** `flowie/CONTROL_GUIDE.md`

## Global Constraints

- Preserve the existing `flowie-server` service behavior and environment contract.
- Run both services as UID/GID `10001:10001`, with a read-only root filesystem, all capabilities dropped, and `no-new-privileges` enabled.
- Never copy deployment secrets, private keys, mutable Control configuration, or SQLite data into an image layer.
- Keep Control HTTPS-only and bind its host-published port to loopback by default.
- Persist Control data independently from the currently non-persistent MQTT protocol state.
- Fail before process launch when the Control configuration path is empty, missing, or unreadable.

---

### Task 1: Control container entrypoint

**Files:**
- Create: `deploy/server/flowie-control-entrypoint.sh`
- Create: `deploy/server/tests/test-flowie-control-entrypoint.sh`

**Interfaces:**
- Consumes: `FLOWIE_CONTROL_CONFIG`, optional `FLOWIE_CONTROL_CHECK`, optional `FLOWIE_CONTROL_KEY_PASSWORD_FILE`, and an optional explicit command.
- Produces: `flowie-control --config <path>` with optional `--check`, or exact pass-through execution when arguments are supplied.

- [x] **Step 1: Write the failing contract test**

  Create a temporary fake `flowie-control`, run the real entrypoint, and assert these literal outcomes: default launch passes `--config <readable-file>`; check mode appends `--check`; an explicit command bypasses default construction; an invalid boolean returns nonzero; a missing config returns nonzero without invoking the fake binary.

- [x] **Step 2: Run the test and verify RED**

  Run: `bash deploy/server/tests/test-flowie-control-entrypoint.sh`

  Expected: failure because `deploy/server/flowie-control-entrypoint.sh` does not exist.

- [x] **Step 3: Implement the minimal entrypoint**

  Use `set -eu`, preserve exact argument pass-through, default `FLOWIE_CONTROL_CONFIG` to `/etc/flowie/control.yml`, validate a regular readable file, import an optional key password file without placing its value in the service environment declaration, accept only `0/1/false/true/no/yes/off/on` for `FLOWIE_CONTROL_CHECK`, and finish with `exec flowie-control --config "$FLOWIE_CONTROL_CONFIG"` plus `--check` when requested.

- [x] **Step 4: Run the test and verify GREEN**

  Run: `bash deploy/server/tests/test-flowie-control-entrypoint.sh`

  Expected: `flowie-control entrypoint contract: PASS` and exit code `0`.

### Task 2: Runtime image packaging

**Files:**
- Modify: `deploy/server/Dockerfile`

**Interfaces:**
- Consumes: installed `flowie_server` and `flowie-control` binaries from `/opt/flowie/$FLOWIE_PROFILE/bin`.
- Produces: `/usr/local/bin/flowie_server`, `/usr/local/bin/flowie-control`, `/usr/local/bin/flowie-control-entrypoint`, and a runtime dependency audit for both executables.

- [x] **Step 1: Copy the installed Control binary into the builder runtime bundle**

  Install `flowie-control` beside `flowie_server` and run `ldd` for both with `/opt/flowie-runtime/lib` active. Fail the image build if either audit contains `not found`.

- [x] **Step 2: Copy Control artifacts into the final image**

  Copy the binary and entrypoint, keep executable permissions, and verify both binaries with final-stage `ldd` checks after `ldconfig`.

- [ ] **Step 3: Build the image when all named source contexts are available**

  Run the documented `docker buildx build` command from `deploy/server/README.md`, using immutable `SOURCE_REVISION` and `--load`.

  Expected: build exit code `0`; both runtime `ldd` manifests contain no `not found`.

  Local status: Docker CLI is unavailable on this workstation, so this remains a deployment-host verification step.

### Task 3: Compose service and container-native configuration

**Files:**
- Modify: `deploy/server/compose.yml`
- Modify: `deploy/server/.env.example`
- Create: `deploy/server/control.yml.example`
- Modify: `.gitignore`
- Modify: `.dockerignore`

**Interfaces:**
- Consumes: the shared Flowie image, a host Control YAML, a host certificate directory, and an explicit Docker secret file.
- Produces: a `flowie-control` Compose service with an independent `flowie-control-data` volume and loopback HTTPS binding through host networking.

- [x] **Step 1: Add the Control service**

  Configure the dedicated entrypoint, host networking, non-root/read-only hardening, explicit config and certificate mounts, the Docker secret file, a writable Control data volume, and the existing TCP healthcheck pointed at `127.0.0.1:8443`.

- [x] **Step 2: Add the container-native example configuration**

  Set listener `127.0.0.1:8443`, certificate paths under `/etc/flowie/certs`, SQLite under `/var/lib/flowie/control`, Dashboard enabled, and local Auth enabled with bounded executor values `4/128/10000` and method `password`.

- [x] **Step 3: Exclude runtime deployment material**

  Exclude `deploy/server/config/`, `deploy/server/certs/`, and `deploy/server/secrets/` from both Git tracking and the Docker build context so runtime private material cannot be copied by the Dockerfile's source-tree `COPY`.

- [ ] **Step 4: Validate Compose rendering**

  Create temporary non-secret placeholder config/certificate paths and a temporary secret file outside tracked sources, then run `docker compose -f deploy/server/compose.yml config` with `FLOWIE_SERVER_IMAGE` set.

  Expected: exit code `0`; rendered services include both `flowie-server` and `flowie-control` without interpolation errors.

  Local status: PyYAML structural checks passed, but Docker Compose rendering remains unverified because Docker CLI is unavailable.

### Task 4: Deployment documentation and verification

**Files:**
- Modify: `deploy/server/README.md`

**Interfaces:**
- Consumes: the image, Compose service, configuration example, fixed `system/admin` bootstrap behavior, and existing Nginx upstream contract.
- Produces: reproducible build, preflight, startup, first-login, persistence, backup, and health verification instructions.

- [x] **Step 1: Document the two-service image contract**

  Explain that the image carries both binaries but Compose runs one process per service; document copying `control.yml.example`, certificate/secret placement, `flowie-control --check`, startup, logs, health, and the fixed first-login password-change requirement.

- [ ] **Step 2: Run focused verification**

  Run:

  ```sh
  bash deploy/server/tests/test-docker-entrypoint.sh
  bash deploy/server/tests/test-flowie-control-entrypoint.sh
  docker compose -f deploy/server/compose.yml config
  ```

  Expected: both shell contracts print `PASS`; Compose exits `0`.

  Local status: both shell contracts passed; Compose remains a deployment-host verification step.

- [x] **Step 3: Inspect the final change set**

  Run: `git diff --check && git status --short && git diff -- deploy/server`

  Expected: no whitespace errors, only planned deployment/test/documentation files changed, and no secrets, keys, generated data, or `.env` files tracked.
