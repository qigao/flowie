# Flowie Dual Image Targets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build two explicit images from one Flowie revision: an unchanged standalone MQTT image and a PicImpact image whose configured `flowie_server` process embeds Flowie Control.

**Architecture:** Keep the existing standalone executable and CLI as `flowie_server`. Add an opt-in configured executable, `flowie_server_control`, that composes the existing worker application and Control runtime; the Control Docker target installs that executable under the runtime name `flowie_server`. BuildKit final targets and flavor labels make the two image contracts unambiguous, while PicImpact promotion tooling refuses any non-Control image.

**Tech Stack:** C11/C++17, CMake presets, TurboFlow, TurboHTTP, Flowie Control, POSIX shell, Docker BuildKit, Docker Compose, PostgreSQL 17, Go/Vite PicImpact images.

**Spec:** `docs/superpowers/specs/2026-08-25-flowie-dual-image-targets-design.md`

## Global Constraints

- Keep the existing standalone `flowie_server` CLI, tuning behavior, tests, and default CMake build unchanged.
- Build the configured server only when `FLOWIE_BUILD_CONFIGURED_SERVER=ON`; missing TurboFlow or Control targets fail configuration.
- Run MQTT and Control in one configured `flowie_server` process; do not supervise two unrelated processes.
- The Control entrypoint always supplies `--require-security`; configuration or dependency failure never falls back to standalone MQTT.
- Use `io.flowie.image.flavor=standalone|control` as the promotion fact and require `control` for PicImpact.
- Keep secrets out of image layers, labels, build logs, command output, and test artifacts.
- Reset only EU dev database/configuration state after a timestamped cold backup; preserve uploads, certificates, Let's Encrypt state, PostgreSQL TLS material, and RustFS/object data.
- Preserve unrelated user changes in `C:/projects/photo-booth/PicImpact`; stage and commit only files changed by this plan.

---

### Task 1: Restore an opt-in configured server executable

**Files:**
- Modify: `CMakeOptions.cmake`
- Modify: `CMakeLists.txt`
- Modify: `CMakeUserPresets.json`
- Modify: `server/CMakeLists.txt`
- Create: `server/flowie_server_control.c`
- Modify: `server/tests/CMakeLists.txt`

**Interfaces:**
- Existing `flowie_server` remains the standalone broker.
- New installed target `flowie_server_control` accepts:

```text
flowie_server_control [--check] [--require-security] [--profile NAME]
  [--control-config PATH] [--protocol-store-path PATH]
  [--storage-backend-plugin PATH]... config.yml graph.flow
```

- `FLOWIE_BUILD_CONFIGURED_SERVER=OFF` does not require `TURBOFLOW_ROOT` or `RULESFORGE_ROOT`.
- `FLOWIE_BUILD_CONFIGURED_SERVER=ON` requires and links `RulesForge`, `TurboFlow::Product`, `TurboFlow::Flow`, `TurboFlow::Config`, `TurboFlow::Socket`, `TurboFlow::HttpClient`, and `TurboFlow::HttpServer`.

- [ ] **Step 1: Add a failing configured-server target contract**

Update `server/tests/CMakeLists.txt` so configured CLI/lifecycle tests refer to
`$<TARGET_FILE:flowie_server_control>` rather than the standalone target. Keep standalone tuning
tests in `server/CMakeLists.txt`. Add a configured smoke test equivalent to:

```cmake
add_test(
  NAME flowie_server_control_check
  COMMAND flowie_server_control --check
          "${PROJECT_SOURCE_DIR}/flowie/examples/flowie.yml"
          "${PROJECT_SOURCE_DIR}/flowie/examples/flowie.flow")
set_tests_properties(flowie_server_control_check
  PROPERTIES LABELS "flowie;configured-server;product-contract")
```

- [ ] **Step 2: Run the focused build to verify RED**

Run:

```powershell
cmake --fresh --preset win-dev-user -DFLOWIE_BUILD_CONFIGURED_SERVER=ON
cmake --build --preset win-dev-user --target flowie_server_control
```

Expected: configuration or target failure because the option, TurboFlow dependency wiring, and
`flowie_server_control` target do not yet exist.

- [ ] **Step 3: Add conditional dependency and target wiring**

Add the option:

```cmake
option(FLOWIE_BUILD_CONFIGURED_SERVER
       "Build the YAML/Graph server with embedded Flowie Control" OFF)
```

When enabled, validate `RULESFORGE_ROOT` and `TURBOFLOW_ROOT`, set their package directories, and
call `find_package(RulesForge CONFIG REQUIRED)` and `find_package(TurboFlow CONFIG REQUIRED)`.
Create `flowie_server_application` from `flowie_server_application.c` and
`flowie_worker_runtime.c`, link the existing Flowie, cluster, Control, TurboFlow, HTTP, CoroNet, and
TurboUtils targets, then create `flowie_server_control` from the new main. Enable
`server/tests/CMakeLists.txt` only for the configured build.

The current cluster link is `flowie_cluster_raft_generation`, not the older monorepo's broader
`flowie_cluster_runtime` assumption. Restore `flowie_supervisor` inside the same configured block
because its tests and process contract require the configured worker executable.

Create `server/flowie_server_control.c` from the last deployed configured main contract. It parses
the interfaces above, calls `flowie_server_application_create/start/stop/destroy`, reports structured
worker/config/Control failures, and waits on SIGINT/SIGTERM. It must not contain standalone listener
flags or fallback behavior.

Add `win-dev-control-user`, `win-release-control-user`, `linux-dev-control-user`, and
`linux-release-control-user` presets that inherit their matching base user preset, set
`FLOWIE_BUILD_CONFIGURED_SERVER=ON`, and provide exact installed RulesForge/TurboFlow roots.

- [ ] **Step 4: Build and run focused configured tests to GREEN**

Run:

```powershell
cmake --fresh --preset win-dev-control-user
cmake --build --preset win-dev-control-user --target flowie_server_control test_flowie_server_application
ctest --preset win-dev-control-user -R '^flowie_server_control_check$|^test_flowie_server_application$|^flowie_server_require_security_' --output-on-failure
```

Expected: configured binary builds, valid YAML/Graph check succeeds, application lifecycle tests
pass, and insecure configuration remains rejected.

- [ ] **Step 5: Prove standalone isolation**

Run:

```powershell
cmake --fresh --preset win-dev-user
cmake --build --preset win-dev-user --target flowie_server
ctest --preset win-dev-user -R '^flowie_server_check_(debug_logging|logging_shutdown_snapshot|custom_tuning)$|^flowie_server_rejects_' --output-on-failure
```

Expected: no TurboFlow root is required and all standalone tests pass unchanged.

- [ ] **Step 6: Commit the executable split**

```powershell
git add CMakeOptions.cmake CMakeLists.txt CMakeUserPresets.json server/CMakeLists.txt server/flowie_server_control.c server/tests/CMakeLists.txt
git commit -m "feat(server): add configured Control executable"
```

---

### Task 2: Split and test the runtime entrypoint contracts

**Files:**
- Keep: `deploy/server/docker-entrypoint.sh`
- Create: `deploy/server/docker-entrypoint-control.sh`
- Modify: `deploy/server/tests/test-docker-entrypoint.sh`
- Create: `deploy/server/tests/test-docker-entrypoint-control.sh`
- Create: `deploy/server/tests/test-healthcheck-control.sh`
- Create: `deploy/server/healthcheck-control.sh`

**Interfaces:**
- `docker-entrypoint.sh` remains the standalone environment-to-listener-CLI adapter.
- `docker-entrypoint-control.sh` requires `FLOWIE_CONFIG`, `FLOWIE_GRAPH`, and
  `FLOWIE_CONTROL_CONFIG`, validates plugin files and `:memory:` protocol storage, and invokes the
  configured runtime with `--require-security`.
- `healthcheck-control.sh` requires both the MQTT TCP listener and an HTTPS response from the
  Control root using `FLOWIE_CONTROL_CA_FILE`.

- [ ] **Step 1: Write failing Control entrypoint and healthcheck tests**

Use a temporary fake `flowie_server` to capture argv. Assert this exact ordered prefix:

```text
--require-security
--profile
flowie
--control-config
/test/control.yml
--protocol-store-path
:memory:
```

and exact final positionals `/test/flowie.yml` and `/test/flowie.flow`. Cover unreadable required
files, a non-memory protocol store, one valid plugin, an unreadable plugin, and explicit command
passthrough. For the healthcheck, fake `nc` and `curl` and prove either listener failure makes the
script fail.

- [ ] **Step 2: Run tests to verify RED**

```powershell
sh deploy/server/tests/test-docker-entrypoint-control.sh
sh deploy/server/tests/test-healthcheck-control.sh
```

Expected: both fail because the Control scripts do not exist.

- [ ] **Step 3: Implement the configured entrypoint and dual readiness check**

The entrypoint builds this command without `eval`:

```sh
set -- flowie_server --require-security --profile "$FLOWIE_PROFILE" \
  --control-config "$FLOWIE_CONTROL_CONFIG" \
  --protocol-store-path "$FLOWIE_PROTOCOL_STORE_PATH"
set -- "$@" "$FLOWIE_CONFIG" "$FLOWIE_GRAPH"
exec "$@"
```

The healthcheck first runs `nc -z -w 2 "$FLOWIE_HEALTH_HOST" "$FLOWIE_HEALTH_PORT"`, then:

```sh
curl --fail --silent --show-error --max-time 3 \
  --cacert "$FLOWIE_CONTROL_CA_FILE" "$FLOWIE_CONTROL_HEALTH_URL" >/dev/null
```

- [ ] **Step 4: Run all shell contracts to GREEN**

```powershell
sh -n deploy/server/docker-entrypoint.sh
sh -n deploy/server/docker-entrypoint-control.sh
sh -n deploy/server/healthcheck.sh
sh -n deploy/server/healthcheck-control.sh
sh deploy/server/tests/test-docker-entrypoint.sh
sh deploy/server/tests/test-docker-entrypoint-control.sh
sh deploy/server/tests/test-healthcheck-control.sh
```

- [ ] **Step 5: Commit the runtime contracts**

```powershell
git add deploy/server/docker-entrypoint-control.sh deploy/server/healthcheck-control.sh deploy/server/tests
git commit -m "feat(deploy): add embedded Control runtime contract"
```

---

### Task 3: Add two final Docker build targets

**Files:**
- Modify: `deploy/server/Dockerfile`
- Modify: `deploy/server/README.md`
- Modify: `deploy/server/.env.example`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

**Interfaces:**
- Build target `standalone` produces label `io.flowie.image.flavor=standalone` and current runtime.
- Build target `control` produces label `io.flowie.image.flavor=control`, installs
  `flowie_server_control` as `/usr/local/bin/flowie_server`, and includes `curl`, dashboard assets,
  configured entrypoint, and Control healthcheck.
- Both images record the same source revision and non-secret dependency revisions.

- [ ] **Step 1: Add Docker target assertions to the remote runbook**

Document two exact build forms:

```sh
docker buildx build \
  --file deploy/server/Dockerfile --target standalone \
  --build-context turbo_utils=../turbo-utils \
  --build-context turbo_parser=../turbo-parser \
  --build-context turbo_net=../turbonet \
  --build-context turbo_db=../../turbodb \
  --build-context turbo_http=../../TurboHTTP \
  --build-context flow_mq=../../flowmq \
  --build-context turbo_raft=../../turboraft \
  --build-arg "SOURCE_REVISION=${FLOWIE_SOURCE_REVISION}" \
  --tag "flowie-standalone:${FLOWIE_SOURCE_REVISION}" --load .

docker buildx build \
  --file deploy/server/Dockerfile --target control \
  --build-context turbo_utils=../turbo-utils \
  --build-context turbo_parser=../turbo-parser \
  --build-context turbo_net=../turbonet \
  --build-context turbo_db=../../turbodb \
  --build-context turbo_http=../../TurboHTTP \
  --build-context flow_mq=../../flowmq \
  --build-context turbo_raft=../../turboraft \
  --build-context rules_forge=../../rulesforge \
  --build-context turbo_flow=../turbo-flow \
  --build-arg "SOURCE_REVISION=${FLOWIE_SOURCE_REVISION}" \
  --tag "flowie-control:${FLOWIE_SOURCE_REVISION}" --load .
```

The Control build additionally supplies `rules_forge` and `turbo_flow` named contexts. Document
label inspection, `ldd` closure checks, and the two distinct entrypoints.

- [ ] **Step 2: Build the current Dockerfile with named targets to verify RED**

On EU, use a run-scoped source bundle and run both documented commands. Expected: unknown target
failure because `standalone` and `control` final stages do not exist.

- [ ] **Step 3: Implement shared runtime base and two final stages**

Keep existing dependency builders cached. Add RulesForge and TurboFlow builders reachable only from
the configured Flowie builder. Add an explicit standalone Flowie builder with
`FLOWIE_BUILD_CONFIGURED_SERVER=OFF` and a Control builder with it enabled.

The final stages must have these labels:

```dockerfile
LABEL io.flowie.image.flavor="standalone"
LABEL io.flowie.image.flavor="control"
```

The Control final stage copies `flowie_server_control` to `/usr/local/bin/flowie_server`; it does
not copy or launch a separate `flowie-control` process. Validate the complete shared-library closure
with `ldd` during the image build.

- [ ] **Step 4: Build and inspect both images on EU**

Use revision-derived immutable tags and assert:

```sh
test "$(docker image inspect --format '{{ index .Config.Labels \"io.flowie.image.flavor\" }}' "$standalone_image")" = standalone
test "$(docker image inspect --format '{{ index .Config.Labels \"io.flowie.image.flavor\" }}' "$control_image")" = control
```

Run `flowie_server --help` in each image and confirm standalone help contains `--host` while Control
help contains `--control-config` and does not contain `--host`.

- [ ] **Step 5: Execute isolated runtime tests**

Start standalone on `127.0.0.1:28883`; prove MQTT 5 QoS 1 publish/subscribe and no listener on
`28443`. Start Control on `127.0.0.1:38883` and `127.0.0.1:38443` with run-scoped SQLite Control
configuration and certificates; prove both listeners, authenticated MQTT, ACL allow/deny, clean
SIGTERM, and no native service creation.

- [ ] **Step 6: Commit Docker targets and documentation**

```powershell
git add deploy/server/Dockerfile deploy/server/README.md deploy/server/.env.example flowie/LINUX_REMOTE_TEST_RUNBOOK.md
git commit -m "feat(deploy): publish standalone and Control image targets"
```

---

### Task 4: Add a PicImpact image-flavor promotion gate

**Files in `C:/projects/photo-booth/PicImpact`:**
- Create: `scripts/verify-flowie-image.sh`
- Create: `scripts/tests/test-verify-flowie-image.sh`
- Modify: `scripts/install-offline.sh`
- Modify: `scripts/package-offline-release.sh`
- Modify: `docs/docker-image-promotion-runbook.md`
- Modify: `docker-compose.flowie.yml`

**Interfaces:**
- `scripts/verify-flowie-image.sh IMAGE [EXPECTED_FLAVOR]` returns success only when the exact local
  image exists and its `io.flowie.image.flavor` label equals the expected value, default `control`.
- Packaging and offline installation reject standalone and unlabeled Flowie images before tagging,
  exporting, database bootstrap, or container replacement.
- `FLOWIE_SERVER_IMAGE` remains the Compose environment key for compatibility; its value points to
  the Control-flavor image.

- [ ] **Step 1: Write a fake-Docker flavor-gate test**

The test supplies a fake `docker` that returns `control`, `standalone`, an empty label, or inspect
failure. Assert success only for `control` when expected and exact exit code `65` for mismatches.

- [ ] **Step 2: Run the test to verify RED**

```powershell
sh scripts/tests/test-verify-flowie-image.sh
```

Expected: failure because `verify-flowie-image.sh` does not exist.

- [ ] **Step 3: Implement and wire the gate**

Implement label inspection without `eval`. Call it immediately after image load in
`install-offline.sh` and before tagging in `package-offline-release.sh`. Add the verifier to the
offline bundle's script list. Update the runbook to require the gate before dev up, release tag,
export, import, and rollback. Update the Compose comment to state that only flavor `control` is
valid; do not add an implicit image default.

- [ ] **Step 4: Run focused deployment-script verification**

```powershell
sh -n scripts/verify-flowie-image.sh
sh -n scripts/install-offline.sh
sh -n scripts/package-offline-release.sh
sh scripts/tests/test-verify-flowie-image.sh
git diff --check
```

- [ ] **Step 5: Commit only the promotion-gate files**

```powershell
git add scripts/verify-flowie-image.sh scripts/tests/test-verify-flowie-image.sh scripts/install-offline.sh scripts/package-offline-release.sh docs/docker-image-promotion-runbook.md docker-compose.flowie.yml
git commit -m "deploy: require Flowie Control image flavor"
```

Do not stage the pre-existing backend, frontend, or MQTT protocol worktree changes.

---

### Task 5: Build the latest PicImpact images and preflight EU replacement

**Files:**
- Verify only: `C:/projects/photo-booth/PicImpact/backend/`
- Verify only: `C:/projects/photo-booth/PicImpact/frontend/`
- Remote evidence root computed by
  `run_id="$(date -u +%Y%m%dT%H%M%SZ)-flowie-dual-image"` and
  `run_root="/root/dev/runs/$run_id"`

**Interfaces:**
- The source sync includes tracked and non-ignored working-tree files without changing local user
  files.
- PicImpact backend/frontend and Flowie Control images receive immutable revision-derived tags.
- The previous three image IDs, container definitions, env files, resolved data paths, and backup
  paths are recorded before replacement.

- [ ] **Step 1: Run local PicImpact focused tests**

Run the smallest changed backend and frontend tests first, then:

```powershell
go test ./...
npm test -- --run
```

Use `backend` and `frontend` as their respective working directories. Record any pre-existing
failure separately; do not edit unrelated code to hide it.

- [ ] **Step 2: Sync the exact working tree to EU without starting services**

```powershell
pwsh ./scripts/deploy-eu.ps1 -HostName eu -UserName root -Environment dev -SkipUp
```

- [ ] **Step 3: Build immutable images in tmux**

Create a UTC run ID on EU. Build `flowie-standalone`, `flowie-control`, `picimpact-backend`, and
`picimpact-frontend`, save full logs under the run directory, and record image IDs and source
revisions. Do not retag `latest` until all isolated image tests pass.

- [ ] **Step 4: Preflight the deployment boundary**

Verify the Flowie Control flavor, Compose config, Control `--check`, certificate readability, exact
loopback port availability, PostgreSQL image availability, current container health, and absence of
an active native Control service. Record the current image IDs and `docker inspect` definitions for
rollback.

---

### Task 6: Cold-backup and reset EU dev database/configuration state

**Files:**
- Move to backup: `/root/data/picimpact-dev/postgres`
- Move to backup when present: `/root/data/picimpact-dev/picimpact/config/picimpact_config.db`
- Preserve: `/root/data/picimpact-dev/picimpact/uploads`
- Preserve: `/root/data/picimpact-dev/flowie/config`
- Preserve: `/root/data/picimpact-dev/flowie/certs`
- Preserve: `/root/data/picimpact-dev/letsencrypt`
- Preserve: `/root/data/picimpact-dev/postgres-tls`

**Interfaces:**
- Backup root is computed from the same run as
  `backup_root="/root/backups/picimpact-dev/${run_id}-pre-dual-image-reset"` and has mode `0700`.
- Every moved source is resolved with `readlink -f` and must remain below
  `/root/data/picimpact-dev`; every destination must remain below the exact backup root.
- All app, Flowie, native Control, and PostgreSQL writers are stopped before moving state.

- [ ] **Step 1: Stop writers in dependency order**

Stop the PicImpact app Compose project, then the Flowie container, then PostgreSQL. Stop the
run-scoped native SQLite Control test unit if it exists. Recheck PIDs and ports `5432`, `8081`,
`8443`, and `18883` before moving state.

- [ ] **Step 2: Resolve and validate exact paths**

Print and record `readlink -f` for the active PGDATA, config DB, uploads, Flowie config/certs,
PostgreSQL TLS, and backup root. Abort if an active source is a symlink outside
`/root/data/picimpact-dev` or a destination escapes the backup root.

- [ ] **Step 3: Move recoverable state into the cold backup**

Move the complete `postgres` directory and the SQLite configuration database when present. Create
fresh directories with the original required ownership/mode through the existing host-preparation
workflow. Do not delete or move preserved paths.

- [ ] **Step 4: Verify backup completeness before initialization**

Record directory sizes, owners, modes, and a SHA-256 checksum of the SQLite configuration backup.
Confirm the old PGDATA is absent from the active location and present under the backup root.

---

### Task 7: Reconfigure, upgrade, and verify EU dev

**Files:**
- Remote env: `/root/code/picimpact/picimpact.dev.env`
- Runtime config: `/root/data/picimpact-dev/flowie/config/control.yml`
- Runtime config: `/root/data/picimpact-dev/flowie/config/flowie.yml`
- Runtime graph: `/root/data/picimpact-dev/flowie/config/flowie.flow`

**Interfaces:**
- Dev env pins the immutable `flowie-control`, backend, and frontend images.
- Control starts once with `schema_mode: migrate`, is provisioned, then restarts with
  `schema_mode: validate`.
- Final state has no native Flowie Control service and exactly one configured Flowie process in the
  container.

- [ ] **Step 1: Initialize PostgreSQL and database boundaries**

Start only PostgreSQL and wait for health. Recreate the `flowie_control` role/database and the
PicImpact schema using existing env-derived credentials without printing passwords. Confirm TLS and
loopback-only publishing.

- [ ] **Step 2: Bootstrap the Control image**

Pin the Control image, set `schema_mode: migrate`, run the configured executable's preflight, and
start `flowie-server`. Wait for both MQTT and Control readiness. Run `provision-flowie.sh`, merge its
generated secrets, change to `schema_mode: validate`, and force-recreate the container.

- [ ] **Step 3: Build/start the latest PicImpact app**

Pin the immutable backend/frontend images and start the PicImpact app Compose project with
`--no-build --pull never`. Wait for backend and Nginx health before any functional verification.

- [ ] **Step 4: Verify process, listeners, and image identity**

Assert:

```text
io.flowie.image.flavor=control
one configured flowie_server process
127.0.0.1:18883 listening
127.0.0.1:8443 listening
no flowie-control.service
no flowie-control-sqlite.service
```

- [ ] **Step 5: Verify Control and MQTT security behavior**

Using the configured CA and provisioned test identities, verify Control readiness, management
authentication, MQTT valid credential success, invalid credential rejection, one ACL-allowed
publish/subscribe, and one ACL-denied operation. Do not print credentials or MQTT payload contents.

- [ ] **Step 6: Verify PicImpact integration**

Verify backend database initialization, Flowie backend service-account synchronization, MQTT
connection, Nginx Control proxy, unauthenticated `/auth/me` and `/api/auth/me` responses of `401`,
and admin/frontend health. Inspect logs for connection refusal, TLS, schema, auth, ACL, and restart
errors.

- [ ] **Step 7: Restart and reverify**

Force-recreate `flowie-server` under `schema_mode: validate`, then recreate the backend. Repeat
listener, Control, MQTT Auth/ACL, backend sync, and container health assertions. Confirm database
state persists across this restart.

- [ ] **Step 8: Record final evidence and rollback command set**

Store source revisions, image IDs, labels, container health/restart counts, non-secret checks,
backup path, and exact rollback image IDs in the run directory. Keep the cold backup until the user
explicitly approves its later removal.

---

### Task 8: Final verification and handoff

**Files:**
- Verify: Flowie worktree
- Verify: PicImpact worktree

- [ ] **Step 1: Run Flowie source gates**

```powershell
git diff --check
cmake --fresh --preset win-dev-user
cmake --build --preset win-dev-user --target flowie_server
ctest --preset win-dev-user -R '^flowie_server_' --output-on-failure
cmake --fresh --preset win-dev-control-user
cmake --build --preset win-dev-control-user --target flowie_server_control test_flowie_server_application
ctest --preset win-dev-control-user -R '^flowie_server_control|^test_flowie_server_application|^test_flowie_control_' --output-on-failure
```

- [ ] **Step 2: Run PicImpact deployment gates**

```powershell
git diff --check
sh scripts/tests/test-verify-flowie-image.sh
docker compose -f docker-compose.flowie.yml config --quiet
docker compose -f docker-compose.prod.yml config --quiet
```

Supply the normal layered env files for the two Compose validation commands.

- [ ] **Step 3: Confirm repository state**

Use `git status --short` and `git log` in both repositories. Confirm only intended commits were
created and all pre-existing PicImpact changes remain present.

- [ ] **Step 4: Publish the result**

Report both image tags/IDs/flavors, local and EU test evidence, EU listener/process state, data
backup path, preserved paths, any unrun verification and its risk, and the exact rollback images.
