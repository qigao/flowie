# Configured TurboDB and Persistent Management Sessions Implementation Plan

> **For Codex:** Execute this plan inline with test-driven development. Preserve unrelated worktree changes and run each named failing test before its production change.

**Goal:** Remove database-driver selection from Flowie server code paths and persist Control management sessions in the configured TurboDB database so valid sessions survive a Control process restart.

**Architecture:** TurboDB remains the storage adapter and receives an `orm_config_t` assembled from configuration-provided `driver` and arbitrary string options. The Control Repository gains a durable session command/query port; its TurboDB adapter owns atomic expiry, per-principal eviction, global LRU eviction, resolve/touch, and revoke operations. The session service retains authentication and opaque-token ownership, hashes the random bearer token with unkeyed BLAKE2b, and stores only the digest plus CSRF and identity metadata. Control schema V7 is intentionally incompatible with V6 and older schemas and follows the existing fail-fast reset/import upgrade policy.

**Tech Stack:** C11, TurboDB Orm C facade, TurboParser JSON/YAML configuration, TurboUtils/TurboSTL, Monocypher, CMake/CTest, TinyTest, POSIX Docker entrypoint tests.

**Compatibility:** Control V6 stores are rejected and must be exported/rebuilt. The raw bearer token is never persisted. SQLite remains available only as an explicit/default configuration value; PostgreSQL is selected by configuration without server-side driver branching. Existing listener and session capacity/TTL meanings remain unchanged.

## Task 1: Specify the V7 durable-session contract with failing tests

**Files:**

- Modify: `control/tests/test_flowie_control_store.c`
- Modify: `control/tests/test_flowie_control_management_session.c`
- Modify: `control/tests/test_flowie_control_repository.c`

1. Add a schema test proving V6 is rejected after V7 is introduced.
2. Add a management-session test that logs in, destroys only the session service, recreates it against the same store, and resolves the original token.
3. Add revoke-after-recreate and expiry-after-recreate assertions.
4. Extend repository validation tests to require the durable session operations.
5. Build and run `test_flowie_control_store`, `test_flowie_control_repository`, and `test_flowie_control_management_session`; confirm the new persistence test fails before implementation.

## Task 2: Add the V7 session schema and TurboDB Repository operations

**Files:**

- Modify: `control/flowie_control_store_internal.h`
- Modify: `control/flowie_control_repository_internal.h`
- Modify: `control/flowie_control_repository.c`
- Modify: `control/flowie_control_store.c`

1. Bump the Control store fingerprint/version to V7 and add `flowie_control_management_session_sequence` plus `flowie_control_management_session` tables and bounded indexes.
2. Define typed Repository records and operations for atomic issue, resolve/touch, and revoke.
3. Implement issue as one transaction: delete expired rows, enforce per-principal oldest-issued eviction, enforce global least-recently-used eviction, allocate a persisted monotonic sequence, and insert the new digest record.
4. Implement resolve/touch as one transaction: reject or delete expired rows, allocate the next sequence, update `last_used`, and return an owned typed record.
5. Implement revoke as a database delete whose absence returns `TURBO_ENOENT`.
6. Bind and validate these operations in the TurboDB Repository adapter.
7. Re-run the three smallest tests and keep them green.

## Task 3: Replace the in-memory session fact source

**Files:**

- Modify: `control/flowie_control_management_session.c`
- Modify: `control/flowie_control_management_session_internal.h`
- Modify: `flowie/CONTROL_GUIDE.md`

1. Remove the in-memory hash map, process-local sequence, mutex, eviction, and random digest key.
2. Keep 256-bit random bearer-token generation and CSRF generation; derive a 32-byte unkeyed BLAKE2b digest so it is stable across process generations.
3. Delegate issue, resolve/touch, and revoke to the Repository session port while preserving the current authentication and current-role re-read semantics.
4. Wipe all token, digest, CSRF, and temporary identity buffers on every exit path.
5. Document that session rows live in the configured Control TurboDB and survive restart until expiry/revocation.
6. Run `test_flowie_control_management_session`, management RPC tests, and the Control test label set.

## Task 4: Make the direct server TurboDB configuration driver-neutral

**Files:**

- Create: `server/flowie_server_turbodb_config_internal.h`
- Create: `server/flowie_server_turbodb_config.c`
- Create: `server/tests/test_flowie_server_turbodb_config.c`
- Modify: `server/flowie_server.c`
- Modify: `server/CMakeLists.txt`
- Modify: `server/cmake/VerifyFlowieServerTurboDB.cmake`

1. Add a bounded parser for a configured driver and JSON object of string TurboDB options; reject empty drivers, non-object JSON, non-string values, duplicates, and more than 16 options.
2. Add direct-server inputs `--protocol-store-driver` and `--protocol-store-options`; retain a SQLite default expressed as default configuration, not a conditional driver selection.
3. Build `orm_config_t` solely from parsed driver/options and pass it to `flowie_protocol_repository_open`.
4. Ensure logs contain the driver but never option values.
5. Add unit tests for SQLite and PostgreSQL option projection and invalid configuration.
6. Update executable tests to pass explicit SQLite JSON options and add a PostgreSQL configuration-validation path where a live database is not required.

## Task 5: Remove SQLite-only handling from the configured Worker path

**Files:**

- Modify: `server/flowie_worker_runtime.c`
- Modify: `server/flowie_worker_runtime_internal.h`
- Modify: `server/flowie_server_application.c`
- Modify: `server/flowie_server_application_internal.h`
- Modify: `server/tests/test_flowie_server_application.c`
- Modify: `flowie/examples/flowie.yml`
- Modify: `flowie/examples/products/flowie-smb.yml`

1. Represent the implicit protocol repository as a borrowed driver/options configuration rather than a SQLite path.
2. Resolve explicit `orm_repository` channels into the same bounded driver/options form without branching on a driver name.
3. Update application tests to prove PostgreSQL configuration reaches TurboDB and invalid option mappings fail with a structured config path.
4. Update examples to use driver plus options while keeping the repository namespace and protocol limits unchanged.

## Task 6: Wire PostgreSQL configuration through the combined container

**Files:**

- Modify: `deploy/server/docker-entrypoint.sh`
- Modify: `deploy/server/tests/test-docker-entrypoint.sh`
- Modify: `deploy/server/.env.example`
- Modify: `deploy/server/compose.yml`
- Modify: `deploy/server/Dockerfile`
- Modify: `deploy/server/README.md`

1. Add environment inputs for the server TurboDB driver and JSON option mapping without logging their contents.
2. Configure the Compose server and Control processes for PostgreSQL using separately scoped credentials/options while allowing the same PostgreSQL service.
3. Keep distinct Flowie protocol and Control schemas/table namespaces inside the selected database.
4. Extend the shell contract test before changing the entrypoint and verify exact argv/env behavior.

## Task 7: Verify SQLite regression and PostgreSQL live behavior

**Files:**

- Modify: `control/tests/test_flowie_control_turbodb_live.c`
- Modify: `deploy/server/tests/run-turbodb-postgres-live.sh`
- Modify: `flowie/SERVER_GUIDE.md`

1. Extend the PostgreSQL live Control test to recreate the session service and resolve/revoke a stored token.
2. Run the smallest Windows tests with the BuildTools developer environment, then the complete Control and registered server test sets.
3. Run the entrypoint shell test in an available POSIX environment.
4. Run the remote Docker PostgreSQL live contract when Docker is available; otherwise report the missing executable as an explicit residual verification gap.
5. Record exact commands and results; do not claim PostgreSQL live success without the live container output.
