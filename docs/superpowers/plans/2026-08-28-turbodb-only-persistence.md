# TurboDB-Only Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove Flowie's direct SQLite/PostgreSQL code and route all persistence through TurboDB while explicitly disabling LSQUIC.

**Architecture:** Add one bounded materialized-result facility to the TurboDB ORM C ABI, then replace Flowie's backend-client providers with a repository adapter that uses only ORM connections, transactions, queries, and results. Preserve repository and RPC contracts; keep backend selection as data passed to TurboDB and fail without fallback.

**Tech Stack:** C11, C++17, TurboDB ORM, TurboUtils CFlow/CMeta/TinyTest, CMake Presets, Docker, PostgreSQL live test.

**Spec:** `docs/superpowers/specs/2026-08-28-turbodb-only-persistence.md`

## Global Constraints

- Flowie must contain no direct SQLite/libpq includes, symbols, package discovery, or target links.
- Database operations, transactions, parameters, results, and diagnostics are owned by TurboDB.
- Keep `flowie_control_repository_t` and management RPC behavior unchanged.
- Do not delete or rewrite existing user, credential, audit, or published-policy data.
- Do not add fallback or dual writes.
- Every Flowie-owned TurboNet configure must set `TURBONET_ENABLE_LSQUIC=OFF`.
- Follow RED-GREEN-REFACTOR and use version-controlled public presets.

---

### Task 1: Add bounded dynamic results to TurboDB ORM

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

- [ ] Write SQLite and fake-PostgreSQL tests for typed/null/blob cells, affected rows, limits,
  transaction execution, and destroy-after-error.
- [ ] Run the focused tests and verify they fail because the result API is absent.
- [ ] Implement the single-owner bounded collector over the existing backend row cursor.
- [ ] Run focused and ABI regression tests to green.
- [ ] Document ownership, limits, errors, and a complete C example.
- [ ] Commit and install the TurboDB development package used by Flowie.

### Task 2: Replace Control backend providers with one TurboDB repository

**Files:**
- Create: `control/flowie_control_turbodb_repository.c`
- Create: `control/flowie_control_turbodb_repository_internal.h`
- Modify: `control/flowie_control_repository.c`
- Modify: `control/flowie_control_repository_internal.h`
- Modify: `control/flowie_control_runtime.c`
- Modify: `control/CMakeLists.txt`
- Delete: `control/flowie_control_store.c`
- Delete: `control/flowie_control_store_internal.h`
- Delete: `control/flowie_control_pgsql_database.c`
- Delete: `control/flowie_control_pgsql_database_internal.h`
- Delete: `control/flowie_control_pgsql_command.c`
- Delete: `control/flowie_control_pgsql_command_internal.h`
- Delete: `control/flowie_control_pgsql_query.c`
- Delete: `control/flowie_control_pgsql_query_internal.h`
- Delete: `control/flowie_control_pgsql_repository.c`
- Delete: `control/flowie_control_pgsql_repository_internal.h`

**Interfaces:**
- Consume the existing runtime SQLite/PostgreSQL configuration and normalize it into
  `orm_config_t`; do not expose backend handles.
- Produce the complete existing `flowie_control_repository_t` operation table.
- Each mutation owns one ORM transaction through validation, revision CAS, audit append, and
  commit. Read snapshots use one connection/transaction and stable ordering.

- [ ] Parameterize the shared repository contracts with a TurboDB provider and verify RED.
- [ ] Implement schema validation/migration and read operations through ORM results.
- [ ] Implement mutation/audit transactions and revision-conflict mapping.
- [ ] Run repository, auth, ACL, management, publish, and runtime tests to green.
- [ ] Switch production composition, remove legacy providers, and rerun the same tests.

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

- [ ] Add a build/runtime test that configures without direct backend packages and verify RED.
- [ ] Move test fixtures and security reads to TurboDB APIs.
- [ ] Remove direct package discovery, target links, includes, and symbols.
- [ ] Configure, build, and run focused tests to green.

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
- [ ] Add the explicit option to container and EU build paths.
- [ ] Run the helper test and container syntax/build target to green.

### Task 5: Documentation and cross-platform verification

**Files:**
- Modify: `flowie/CONTROL_GUIDE.md`
- Modify: `flowie/ARCHITECTURE.md`
- Modify: `flowie/ADR_CONTROL_STORE_PROVIDER.md`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

**Interfaces:**
- Documentation describes TurboDB as the only database boundary and backend drivers as package
  capabilities, not Flowie dependencies.

- [ ] Remove obsolete direct-provider instructions and update configuration examples.
- [ ] Run the smallest Windows preset targets, then adjacent Control/Flowie tests.
- [ ] Run the EU PostgreSQL repository contract and container gate.
- [ ] Verify no Flowie source or build target directly references SQLite/libpq and no Flowie build
  enables LSQUIC.
- [ ] Record exact commands, test counts, and residual risks.
