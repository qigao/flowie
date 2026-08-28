# TurboDB-Only Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove Flowie's direct SQLite/PostgreSQL code and route all persistence through TurboDB while explicitly disabling LSQUIC.

**Architecture:** Use the merged bounded materialized-result facility in the TurboDB ORM C ABI, then replace Flowie's backend-client providers with one repository adapter that receives `orm_config_t` and uses only ORM connections, transactions, queries, and results. Preserve repository and RPC contracts; expose only `storage.turbodb` and fail without fallback.

**Tech Stack:** C11, C++17, TurboDB ORM, TurboUtils CFlow/CMeta/TinyTest, CMake Presets, Docker.

**Spec:** `docs/superpowers/specs/2026-08-28-turbodb-only-persistence.md`

## Global Constraints

- Flowie must contain no direct SQLite/libpq includes, symbols, package discovery, or target links.
- Database operations, transactions, parameters, results, and diagnostics are owned by TurboDB.
- Keep `flowie_control_repository_t` and management RPC behavior unchanged.
- Delete the old SQLite/PostgreSQL configuration and provider code without a compatibility parser
  or migration path, as explicitly requested.
- Do not add fallback or dual writes.
- Every Flowie-owned TurboNet configure must set `TURBONET_ENABLE_LSQUIC=OFF`.
- Follow RED-GREEN-REFACTOR and use version-controlled public presets.

---

### Task 1: Verify bounded dynamic results in TurboDB ORM

**Files:**
- Modify: `../turbodb/orm/include/orm/orm.h`
- Modify: `../turbodb/orm/src/abi/orm_core.c`
- Modify: `../turbodb/orm/src/abi/orm_internal.h`
- Modify: `../turbodb/orm/tests/flow/orm_sqlite_flow_test.c`
- Modify: `../turbodb/orm/tests/flow/orm_postgres_flow_test.c`
- Modify: `../turbodb/orm/readme.md`

**Interfaces:**
- Produce opaque `orm_result_t` with `orm_query_execute()` and
  `orm_query_execute_in_transaction()`.
- Produce row/column counts, affected rows, null/type inspection, and borrowed cell views.
- `orm_result_destroy()` is the sole release operation; every returned view expires at destroy.
- Retained rows and bytes use `orm_config_t` limits and fail atomically with
  `ORM_STATUS_LIMIT_EXCEEDED`.

- [x] Confirm the merged result API exposes typed/null/blob cells, affected rows, limits,
  transaction execution, and single-owner destruction.
- [x] Configure and build TurboDB from merged `origin/main`.
- [x] Run 11 focused ORM tests to green.

### Task 2: Replace Control backend providers with one TurboDB repository

**Files:**
- Modify: `control/flowie_control_database.c`
- Modify: `control/flowie_control_store.c`
- Modify: `control/flowie_control_runtime.c`
- Modify: `control/flowie_control_config.c`
- Modify: `control/CMakeLists.txt`
- Delete: `control/flowie_control_pgsql_database.c`
- Delete: `control/flowie_control_pgsql_database_internal.h`
- Delete: `control/flowie_control_pgsql_command.c`
- Delete: `control/flowie_control_pgsql_command_internal.h`
- Delete: `control/flowie_control_pgsql_query.c`
- Delete: `control/flowie_control_pgsql_query_internal.h`
- Delete: `control/flowie_control_pgsql_repository.c`
- Delete: `control/flowie_control_pgsql_repository_internal.h`

**Interfaces:**
- Consume only `storage.turbodb` and materialize one bounded `orm_config_t`; do not expose backend
  handles or retain old configuration.
- Produce the complete existing `flowie_control_repository_t` operation table.
- Each mutation owns one ORM transaction through validation, revision CAS, audit append, and
  commit. Read snapshots use one connection/transaction and stable ordering.

- [x] Parameterize the shared repository contracts with one `orm_config_t` and verify RED.
- [x] Route schema validation and reads through TurboDB ORM results.
- [x] Preserve mutation/audit transactions and revision-conflict mapping through TurboDB.
- [x] Run repository, auth, ACL, management, publish, and runtime tests to green.
- [x] Switch production composition, remove legacy providers, and rerun the same tests.

### Task 3: Remove direct backend access from protocol/security tests and build graph

**Files:**
- Modify: `flowie/tests/test_flowie_protocol_repository_pgsql_live.c`
- Modify: `flowie/tests/CMakeLists.txt`
- Replace: `control/flowie_security_turbodb.c`
- Replace: `control/flowie_security_turbodb.h`
- Modify: `CMakeLists.txt`
- Modify: `CMakeOptions.cmake`
- Modify: `control/tests/test_flowie_control_store.c`
- Replace/Delete: `control/tests/test_flowie_control_pgsql_database.c`
- Replace/Delete: `control/tests/test_flowie_control_pgsql_database_live.c`

**Interfaces:**
- Test setup and cleanup use TurboDB, not backend client APIs.
- Security policy loading uses the repository/TurboDB boundary and preserves bounded rule loads.

- [x] Add config/runtime tests for the single TurboDB boundary and verify RED.
- [x] Move test fixtures and security reads to TurboDB APIs.
- [x] Remove direct package discovery, target links, includes, and symbols.
- [x] Configure, build, and run focused tests to green.

### Task 4: Disable LSQUIC in every Flowie-owned build

**Files:**
- Modify: `deploy/server/Dockerfile`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`
- Modify: the checked-in EU execution example/helper referenced by the runbook, if separate.
- Modify: `deploy/server/tests/test-docker-entrypoint.sh` or the nearest build-argument contract test.

**Interfaces:**
- Every TurboNet configure invoked by Flowie passes
  `-DTURBONET_ENABLE_LSQUIC=OFF`.

- [ ] Add an executable helper test that fails when the TurboNet configure omits the option.
- [ ] Run it and verify RED.
- [x] Keep the explicit option in container and EU build paths.
- [ ] Run the helper test and container syntax/build target to green.

### Task 5: Documentation and cross-platform verification

**Files:**
- Modify: `flowie/CONTROL_GUIDE.md`
- Modify: `flowie/ARCHITECTURE.md`
- Delete: `flowie/ADR_CONTROL_STORE_PROVIDER.md`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

**Interfaces:**
- Documentation describes TurboDB as the only database boundary and backend drivers as package
  capabilities, not Flowie dependencies.

- [x] Remove obsolete direct-provider instructions and update configuration examples.
- [x] Add a TurboDB-only PostgreSQL live repository contract; no backend client API appears in the
  Flowie test.
- [x] Run the smallest Windows preset targets, then adjacent Control/Flowie tests.
- [x] Run the EU TurboDB Control contract and run-scoped PostgreSQL container gate.
  - 2026-08-29: Flowie `c7936f4` plus the schema-v6 BIGINT patch ran both PostgreSQL live tests
    against `postgres:17.6-alpine3.22`; 2/2 passed and the runner left zero test containers.
- [x] Verify no Flowie source or build target directly references SQLite/libpq and no Flowie build
  enables LSQUIC.
- [x] Record exact commands, test counts, and residual risks.
