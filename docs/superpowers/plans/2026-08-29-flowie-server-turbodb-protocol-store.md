# Flowie Server TurboDB Protocol Store Implementation Plan

> **For agentic workers:** Execute each task in order and preserve the server's current direct-listener CLI contract.

**Goal:** Make the production `flowie_server` executable persist MQTT protocol state through the Flowie TurboDB/Orm repository instead of endpoint-owned memory.

**Architecture:** `flowie_server` owns one `flowie_protocol_repository_t`, opens it before endpoint creation, injects it through `flowie_endpoint_bindings_t`, destroys the endpoint before closing the repository, and fails startup when the repository cannot be opened. The repository remains the single fact source for sessions, subscriptions, inflight messages, retained publications, wills, and principal snapshots. Application payload dispatch remains unchanged.

**Tech Stack:** C11, Flowie endpoint API, TurboDB Orm C facade, CMake/CTest, POSIX container entrypoint tests.

**Compatibility:** Existing listener and capacity flags retain their meaning. A new `--protocol-store` path is added; native execution defaults to `flowie-protocol.sqlite3`, while the container defaults to `/var/lib/flowie/flowie-protocol.sqlite3` on its existing persistent volume. Database/schema failures are fatal and do not fall back to memory.

## Task 1: Add an executable-level persistence contract test

**Files:**

- Create: `server/cmake/VerifyFlowieServerTurboDB.cmake`
- Modify: `server/CMakeLists.txt`

1. Run `flowie_server --check --protocol-store <build-tree-temp-file>`.
2. Require successful exit, the standard check result, and a non-empty database file.
3. Run the test before production changes and confirm it fails because the CLI/database wiring is absent.

## Task 2: Own and inject the TurboDB repository

**Files:**

- Modify: `server/flowie_server.c`
- Modify: `server/CMakeLists.txt`

1. Add `--protocol-store` and validate that its value is non-empty.
2. Build an `orm_config_t` for the TurboDB SQLite driver without including or calling a database driver's native API.
3. Open the V2 protocol repository with endpoint-derived capacity limits and schema creation enabled.
4. Inject the caller-owned repository using `flowie_endpoint_core_create_ex`.
5. Centralize cleanup so every post-open error destroys the endpoint before closing the repository.
6. Make existing check tests use `:memory:` so only the dedicated persistence contract creates a file.
7. Build and run focused server tests.

## Task 3: Persist by default in the container

**Files:**

- Modify: `deploy/server/tests/test-docker-entrypoint.sh`
- Modify: `deploy/server/docker-entrypoint.sh`
- Modify: `deploy/server/Dockerfile`

1. Extend the entrypoint contract test with `FLOWIE_PROTOCOL_STORE` and first confirm it fails.
2. Map `FLOWIE_PROTOCOL_STORE` to `--protocol-store`.
3. Default the image to `/var/lib/flowie/flowie-protocol.sqlite3`, using the existing writable volume.
4. Run the entrypoint contract test.

## Task 4: Synchronize documentation and verify regressions

**Files:**

- Modify: `deploy/server/README.md`
- Modify: `flowie/SERVER_GUIDE.md`

1. Document the new CLI/environment contract, lifecycle, fact scope, and fail-fast behavior.
2. Run formatting/diff checks, focused CTest, adjacent Flowie tests, and the configured preset's broader regression set where available.
3. Report exact commands, outputs, unverified platform-specific paths, and remaining risks. Do not commit or push unless explicitly requested.
