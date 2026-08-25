# Flowie Control ORM Schema Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move Flowie Control SQLite/PostgreSQL persistence behind one schema-first TurboDB ORM adapter while preserving the existing repository, transaction, migration, audit, timeout, shutdown, and security contracts.

**Architecture:** TurboDB first gains structured PostgreSQL diagnostics, real live coverage, and generated-C composite-key support. Flowie then defines one Control schema, builds a bounded pool of ORM connections, implements the existing `flowie_control_repository_t` operation table on that pool, proves differential parity with the current providers, and switches composition without dual-writing or fallback.

**Tech Stack:** C11, C++17, TurboDB::ORM, TurboParser TBE schema tools/DataBind, TurboUtils threads/errors/TinyTest, SQLite3, PostgreSQL 17/libpq, CMake presets, CTest.

**Spec:** `docs/superpowers/specs/2026-08-25-flowie-control-orm-schema-design.md`

## Global Constraints

- Keep `flowie_control_repository_t` as the only storage dependency of Control services.
- Use separate `flowie_control` and `flowie_protocol` namespaces; never share a transaction between them.
- Select exactly one provider per process; no SQLite/PostgreSQL dual write and no failure fallback.
- Preserve `schema_mode: validate|migrate`, schema version/fingerprint validation, revision CAS, same-transaction audit, stable pagination, and existing public configuration.
- Keep pool capacity in `1..64`, use timeout-based acquisition, reject new leases during close, wake waiters, and destroy only after all leases return.
- Keep passwords separate from public conninfo and never log passwords, credentials, SQL parameters, tokens, or full connection strings.
- Use INFO only for startup/shutdown/schema milestones; rate-limit retryable WARN events; keep redacted backend details at DEBUG.
- Build and test through version-controlled `CMakeUserPresets.json`; run the smallest target/filter first.
- Follow strict RED-GREEN-REFACTOR for every behavior change.

---

### Task 1: Preserve PostgreSQL SQLSTATE through the TurboDB ORM ABI

**Files:**
- Modify: `../turbodb/orm/include/orm/orm.h`
- Modify: `../turbodb/orm/src/abi/orm_c_internal.hpp`
- Modify: `../turbodb/orm/src/abi/orm_c.cpp`
- Modify: `../turbodb/orm/include/orm/dbs/postgres/pg_detail.hpp`
- Modify: `../turbodb/orm/src/dbs/postgres/backend.cpp`
- Modify: `../turbodb/orm/tests/abi/c_api_fake_libpq.cpp`
- Modify: `../turbodb/orm/tests/abi/c_api_test.c`
- Modify: `../turbodb/orm/README.md`

**Interfaces:**
- Extend `orm_error_t` at the tail with `char backend_code[ORM_C_BACKEND_CODE_CAPACITY]`.
- Keep the existing `orm_error_init()` symbol limited to the legacy prefix and add
  `orm_error_init_s(orm_error_t *error, uint32_t struct_size)` for callers that opt
  into the extended diagnostic. This prevents an updated library from overrunning
  an error object allocated by an older binary.
- Add `const char *orm_error_backend_code(const orm_error_t *error)`; it returns `""` when unavailable.
- Carry optional backend code in internal `status_error`; PostgreSQL stores the five-character SQLSTATE.

- [ ] **Step 1: Write ABI and PostgreSQL diagnostic tests**

Add tests that initialize a full `orm_error_t` with
`orm_error_init_s(&error, sizeof(error))`, script fake libpq with SQLSTATE
`23505`, execute a failing query, and assert:

```c
require_status(status, ORM_STATUS_BUSY, &error,
               "map PostgreSQL unique violation");
require_true(strcmp(orm_error_backend_code(&error), "23505") == 0,
             "PostgreSQL SQLSTATE was not preserved");
```

Also pass a legacy prefix-sized error buffer to an internal test hook and assert no byte beyond the reported size changes.

- [ ] **Step 2: Run the focused test and verify RED**

Run the TurboDB public development preset and target:

```powershell
cmake --fresh --preset win-dev-user
cmake --build --preset win-dev-user --target orm_postgres_c_api_test
ctest --preset win-dev-user -R '^orm_postgres_c_api$' --output-on-failure
```

Expected: compile failure because `orm_error_backend_code` and the backend-code field do not exist.

- [ ] **Step 3: Implement structured backend diagnostics and status mapping**

Add a tail field without changing earlier offsets:

```c
#define ORM_C_BACKEND_CODE_CAPACITY UINT32_C(16)

typedef struct orm_error {
  uint32_t struct_size;
  orm_status_t status;
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  char backend_code[ORM_C_BACKEND_CODE_CAPACITY];
} orm_error_t;
```

Map PostgreSQL SQLSTATE before throwing. Keep the existing ORM status enum stable;
Flowie uses the structured backend code to refine domain error mapping:

```cpp
orm_status_t postgres_status(std::string_view code) noexcept {
  if (code == "23505" || code == "40001" || code == "40P01" ||
      code == "55P03" || code == "23503")
    return ORM_STATUS_BUSY;
  if (code.size() >= 2 && code.substr(0, 2) == "08") return ORM_STATUS_CONNECTION_ERROR;
  return ORM_STATUS_SQL_ERROR;
}
```

Read SQLSTATE with `PQresultErrorField(result, PG_DIAG_SQLSTATE)`, preserve it in `status_error`, and copy it only when `error->struct_size` reaches the new tail field.

- [ ] **Step 4: Run RED test to GREEN and regress the ORM ABI suite**

```powershell
cmake --build --preset win-dev-user --target orm_postgres_c_api_test
ctest --preset win-dev-user -R '^orm_postgres_c_api$|^orm_abi_' --output-on-failure
```

Expected: all selected tests pass and the fake libpq result/connection counts remain balanced.

- [ ] **Step 5: Commit TurboDB diagnostic support**

```powershell
git add orm/include/orm/orm.h orm/src/abi/orm_c_internal.hpp orm/src/abi/orm_c.cpp orm/include/orm/dbs/postgres/pg_detail.hpp orm/src/dbs/postgres/backend.cpp orm/tests/abi/c_api_fake_libpq.cpp orm/tests/abi/c_api_test.c orm/README.md
git commit -m "feat(orm): expose PostgreSQL backend diagnostics"
```

### Task 2: Add a real PostgreSQL TurboDB ORM integration gate

**Files:**
- Modify: `../turbodb/orm/CMakeLists.txt`
- Create: `../turbodb/orm/tests/integration/postgres_live_test.c`
- Modify: `../turbodb/orm/README.md`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

**Interfaces:**
- Add cache option `ORM_POSTGRES_LIVE_TESTS` defaulting to `OFF`.
- Live test reads `TURBODB_ORM_PGSQL_TEST_CONNINFO` and optional `PGPASSWORD`; secrets are not printed.
- The test uses a process-unique `orm_pg_live_<pid>_<timestamp>` schema and always drops it.

- [ ] **Step 1: Register a disabled live target and write the failing behavior test**

The TinyTest spec must cover real connect, raw DDL, parameter binding, binary round trip, commit, destructor rollback, isolation, unique violation (`23505`), statement timeout (`57014`), result limits, and cleanup. The first case begins with:

```c
spec("TurboDB ORM PostgreSQL live") {
  it("round trips parameters, bytea, commit and rollback") {
    const char *conninfo = getenv("TURBODB_ORM_PGSQL_TEST_CONNINFO");
    check_not_null(conninfo);
    /* connect through driver=postgresql, create run-scoped schema, assert rows */
  }
}
```

- [ ] **Step 2: Configure without the live option and verify it is disabled**

```powershell
cmake --fresh --preset win-dev-user
ctest --preset win-dev-user -N -R '^orm_postgres_live$'
```

Expected: the test is present but disabled, or absent with a configure message explicitly stating that the live gate is off.

- [ ] **Step 3: Implement the live connection helper**

Teach the PG backend to accept an ORM option named `conninfo` by passing its value as libpq `dbname` with `expand_dbname=1`; keep `password` a distinct option. Reject simultaneous `conninfo` plus host/dbname/user coordinate options because the precedence would be ambiguous.

- [ ] **Step 4: Run the suite against EU PostgreSQL through an SSH loopback tunnel**

Use local port `15432`, capture `PGPASSWORD` without printing it, and set:

```text
TURBODB_ORM_PGSQL_TEST_CONNINFO=host=127.0.0.1 port=15432 dbname=picimpact user=picimpact sslmode=disable connect_timeout=5
```

Run `ctest --preset win-dev-user -R '^orm_postgres_live$' --output-on-failure`. Expected: all cases pass and a subsequent namespace query finds zero `orm_pg_live_%` schemas.

- [ ] **Step 5: Document the Linux/EU command and commit**

```powershell
git add orm/CMakeLists.txt orm/tests/integration/postgres_live_test.c orm/README.md
git commit -m "test(orm): add PostgreSQL live integration gate"
```

Commit the Flowie runbook update separately in the Flowie repository:

```powershell
git add flowie/LINUX_REMOTE_TEST_RUNBOOK.md
git commit -m "docs: add ORM PostgreSQL remote gate"
```

### Task 3: Generate C facades for direct composite identities

**Files:**
- Modify: `../turbodb/orm/tools/orm_schema_generator.cpp`
- Modify: `../turbodb/orm/tools/templates/orm_c_facade.mustache`
- Modify: `../turbodb/orm/tests/schema/c_facade_fixture.schema`
- Modify: `../turbodb/orm/tests/abi/orm_schema_generated_c_test.c`
- Modify: `../turbodb/orm/README.md`

**Interfaces:**
- Generated C `find/remove/update` signatures accept one typed argument per direct primary-key field, in schema declaration order.
- Single-key generated names/signatures remain source-compatible.
- Embedded identifiers remain rejected for the C facade in this task.

- [ ] **Step 1: Add a direct-composite schema fixture and compile assertions**

Add:

```text
[table(memberships)] message Membership {
  [id(1)] string domain_id;
  [id(1)] string user_id;
  [id(1)] string group_id;
  uint64 revision;
}
```

Call the desired generated API:

```c
Membership_t row;
uint8_t found = 0;
Membership_init(&row);
check_equal(CStore_Membership_orm_find(connection, domain, user, group,
                                       &row, &found, &error), ORM_STATUS_OK);
```

- [ ] **Step 2: Build the generator test and verify RED**

```powershell
cmake --build --preset win-dev-user --target orm_schema_generated_c_test
```

Expected: schema generation rejects multiple direct primary keys or generated signature mismatch.

- [ ] **Step 3: Render ordered composite predicates and parameters**

Change generator context from singular `primary_key` to ordered `primary_keys`, emitting one `orm_query_where` per key. Generated update/remove must include every key and must never perform a partial-key mutation.

- [ ] **Step 4: Run generator, C facade, and validator suites to GREEN**

```powershell
ctest --preset win-dev-user -R '^orm_schema_generated_c$|^orm_schema_generated_metadata$|^orm_schema_validator$' --output-on-failure
```

- [ ] **Step 5: Commit composite C facade support**

```powershell
git add orm/tools/orm_schema_generator.cpp orm/tools/templates/orm_c_facade.mustache orm/tests/schema/c_facade_fixture.schema orm/tests/abi/orm_schema_generated_c_test.c orm/README.md
git commit -m "feat(orm): generate C facades for composite keys"
```

### Task 4: Define the canonical Flowie Control schema and compatibility fingerprint

**Files:**
- Create: `control/schema/flowie_control.schema`
- Create: `control/flowie_control_schema.c`
- Create: `control/flowie_control_schema_internal.h`
- Create generated-at-build outputs under: `build/<preset>/control/generated/`
- Modify: `control/CMakeLists.txt`
- Create: `control/tests/test_flowie_control_schema.c`
- Modify: `control/tests/test_flowie_control_store.c`
- Modify: `control/tests/test_flowie_control_pgsql_database_live.c`

**Interfaces:**
- Export internally:

```c
#define FLOWIE_CONTROL_SCHEMA_VERSION UINT32_C(1)
const char *flowie_control_schema_fingerprint(void);
int flowie_control_schema_migrate(orm_connection_t *connection,
                                  const char *driver,
                                  const char *namespace_name);
int flowie_control_schema_validate(orm_connection_t *connection,
                                   const char *driver,
                                   const char *namespace_name,
                                   uint32_t *version_out);
```

- The `.schema` contains all current Control entities: schema version, meta, domain, user account, credential, security group, security role, membership, user role, policy draft, ACL bundle, ACL rule, publish result, and audit.
- SQLite resolves tables to `flowie_control_<table>`; PostgreSQL resolves them to safely quoted `<schema>.<table>`.

- [ ] **Step 1: Write schema validation and deployed-layout compatibility tests**

Tests assert version `1`, one normalized fingerprint across SQLite and PG, exact table/column/key inventory, rejection of an unsafe namespace, migrate-then-validate, validate-without-DDL failure on an empty database, and compatibility with databases created by the current providers.

- [ ] **Step 2: Run focused tests and verify RED**

```powershell
cmake --build --preset win-dev-user --target test_flowie_control_schema
ctest --preset win-dev-user -R '^test_flowie_control_schema$' --output-on-failure
```

Expected: target/configure failure because schema generation and schema lifecycle APIs do not exist.

- [ ] **Step 3: Implement normalized manifest and backend DDL rendering**

Use the generated C model/facade for entity fields and keys. Keep migration DDL in one schema module, with only backend type/namespace branches. Compute the checked-in expected fingerprint from the normalized ordered entity/field/key/index manifest and verify it at test time; raw SQLite/PG SQL text is not fingerprint input.

- [ ] **Step 4: Run SQLite and EU PostgreSQL schema tests to GREEN**

Run local SQLite first, then the PG live binary through the tunnel. Assert all run-scoped PG schemas are dropped.

- [ ] **Step 5: Commit the canonical schema**

```powershell
git add control/schema/flowie_control.schema control/flowie_control_schema.c control/flowie_control_schema_internal.h control/CMakeLists.txt control/tests/test_flowie_control_schema.c control/tests/test_flowie_control_store.c control/tests/test_flowie_control_pgsql_database_live.c
git commit -m "feat(control): define canonical ORM schema"
```

### Task 5: Implement the bounded Control ORM connection pool

**Files:**
- Create: `control/flowie_control_orm_pool.c`
- Create: `control/flowie_control_orm_pool_internal.h`
- Modify: `control/CMakeLists.txt`
- Create: `control/tests/test_flowie_control_orm_pool.c`

**Interfaces:**

```c
typedef struct flowie_control_orm_pool_s flowie_control_orm_pool_t;
typedef struct flowie_control_orm_lease_s {
  size_t size;
  flowie_control_orm_pool_t *pool;
  orm_connection_t *connection;
  size_t slot;
} flowie_control_orm_lease_t;

typedef int (*flowie_control_orm_secret_fn)(void *ctx, char *output,
                                            size_t output_capacity);

typedef struct flowie_control_orm_pool_config_s {
  size_t size;
  const char *driver;
  const orm_option_t *public_options;
  size_t public_option_count;
  flowie_control_orm_secret_fn password;
  void *password_ctx;
  size_t capacity;
  int acquire_timeout_ms;
} flowie_control_orm_pool_config_t;

int flowie_control_orm_pool_create(const flowie_control_orm_pool_config_t *config,
                                   flowie_control_orm_pool_t **out);
int flowie_control_orm_pool_acquire(flowie_control_orm_pool_t *pool,
                                    flowie_control_orm_lease_t *lease);
int flowie_control_orm_pool_release(flowie_control_orm_lease_t *lease);
int flowie_control_orm_pool_close(flowie_control_orm_pool_t *pool, int timeout_ms);
int flowie_control_orm_pool_destroy(flowie_control_orm_pool_t *pool);
```

**Pool protocol:**
- Unit: a fixed slot owning one `orm_connection_t *`.
- Owner: pool creates/destroys slots; a successful lease exclusively borrows one connection.
- Topology: MPMC callers protected by one TurboUtils mutex/condition; connections themselves remain single-thread domains.
- Capacity: fixed `1..64`; no runtime growth.
- Backpressure: acquire waits until configured timeout and returns `TURBO_ETIMEDOUT`.
- State: `FREE -> LEASED -> FREE`; close changes pool `OPEN -> CLOSING -> CLOSED`.
- Shutdown: reject new acquire, wake waiters, wait for all leases, then disconnect; destroy before CLOSED returns `TURBO_EBUSY`.
- Observability: current/peak leases, waits, timeouts and reconnect failures are counters; no per-acquire INFO logs.

- [ ] **Step 1: Write state-machine and concurrency tests**

Cover invalid capacity, capacity one, capacity 64, capacity+1 rejection, exhaustion timeout, release wakeup, close wakeup, leaked lease close timeout, duplicate release, stats, partial-create cleanup, and concurrent exclusive ownership.

- [ ] **Step 2: Run focused test and verify RED**

```powershell
cmake --build --preset win-dev-user --target test_flowie_control_orm_pool
ctest --preset win-dev-user -R '^test_flowie_control_orm_pool$' --output-on-failure
```

- [ ] **Step 3: Implement the minimal mutex/condition pool**

Allocate the fixed slot array with checked multiplication, open all connections
before publication, never invoke the secret callback under the pool lock, and
clear each bounded password scratch buffer immediately after `orm_connect()`.
Reconnect obtains a fresh secret through the callback; the pool never retains a
password or a secret-bearing connection string.

- [ ] **Step 4: Run normal, stress, and sanitizer pool tests**

Run the focused test repeatedly on Windows Debug and Linux ASan/TSan where available. Expected: no leaked lease, race, deadlock, or unbounded wait.

- [ ] **Step 5: Commit the pool**

```powershell
git add control/flowie_control_orm_pool.c control/flowie_control_orm_pool_internal.h control/CMakeLists.txt control/tests/test_flowie_control_orm_pool.c
git commit -m "feat(control): add bounded ORM connection pool"
```

### Task 6: Implement ORM-backed Control queries

**Files:**
- Create: `control/flowie_control_orm_query.c`
- Create: `control/flowie_control_orm_query_internal.h`
- Modify: `control/CMakeLists.txt`
- Create: `control/tests/test_flowie_control_orm_query.c`
- Modify: `control/tests/flowie_control_repository_contract.h`
- Modify: `control/tests/flowie_control_auth_repository_contract.h`

**Interfaces:**
- Query object borrows the pool and acquires exactly one lease for each repository call.
- Implement domain/user/group/role pagination; credential state/verify/resolve; current revision; principal and external-principal snapshots; effective groups/roles; policy validation/status/rule list/bundle load; audit list/count.
- Returned bundles own copied strings/arrays and are released through the existing repository operation table.

- [ ] **Step 1: Parameterize read contracts with the ORM provider**

Add an ORM provider factory to the existing shared contract harness. New cases assert stable exclusive cursors, empty pages, exact page boundaries, snapshot consistency, NULL/binary handling, capacity rejection, and cleanup after a query error.

- [ ] **Step 2: Run the ORM read contract and verify RED**

```powershell
ctest --preset win-dev-user -R '^test_flowie_control_orm_query$' --output-on-failure
```

- [ ] **Step 3: Implement typed query helpers**

Use generated entity/table names and ORM parameter binding. Do not concatenate caller-provided identifiers or values. Apply explicit `ORDER BY` for every paginated query and enforce all configured result bounds before allocation.

- [ ] **Step 4: Run SQLite and PostgreSQL read contracts to GREEN**

Run the same contract once with driver `sqlite`, then against the EU PG test schema. Expected: identical domain results and error codes.

- [ ] **Step 5: Commit the read adapter**

```powershell
git add control/flowie_control_orm_query.c control/flowie_control_orm_query_internal.h control/CMakeLists.txt control/tests/test_flowie_control_orm_query.c control/tests/flowie_control_repository_contract.h control/tests/flowie_control_auth_repository_contract.h
git commit -m "feat(control): add ORM repository queries"
```

### Task 7: Implement ORM-backed Control commands and audit transactions

**Files:**
- Create: `control/flowie_control_orm_command.c`
- Create: `control/flowie_control_orm_command_internal.h`
- Modify: `control/CMakeLists.txt`
- Create: `control/tests/test_flowie_control_orm_command.c`
- Modify: `control/tests/flowie_control_management_repository_contract.h`

**Interfaces:**
- Implement domain/user/group/role creation and state changes, memberships, user roles, credential generate/rotate/revoke, policy draft put/delete/publish, and idempotent publish results.
- Each command uses one ORM transaction containing validation reads, expected-revision CAS, mutation, global revision increment, and audit insert.

- [ ] **Step 1: Add failing command/audit parity tests**

For every command family, assert success, stale revision conflict, missing parent, duplicate identity, domain isolation, capacity limit, idempotent retry, audit count/content, and no state/audit change after forced rollback.

- [ ] **Step 2: Run focused command tests and verify RED**

```powershell
ctest --preset win-dev-user -R '^test_flowie_control_orm_command$' --output-on-failure
```

- [ ] **Step 3: Implement one-lease, one-transaction command helpers**

Map ORM statuses using structured backend codes. Retry serialization/deadlock failures only at the outer command boundary with a fixed bounded attempt count already defined by the current provider contract; do not retry unknown commit outcomes.

- [ ] **Step 4: Run command contracts and concurrency conflicts to GREEN**

Run SQLite and PG variants, including two concurrent writers using the same expected revision. Exactly one succeeds; the other returns the existing conflict error, and one audit row is appended.

- [ ] **Step 5: Commit command support**

```powershell
git add control/flowie_control_orm_command.c control/flowie_control_orm_command_internal.h control/CMakeLists.txt control/tests/test_flowie_control_orm_command.c control/tests/flowie_control_management_repository_contract.h
git commit -m "feat(control): add transactional ORM commands"
```

### Task 8: Compose the ORM repository and switch provider selection

**Files:**
- Create: `control/flowie_control_orm_repository.c`
- Create: `control/flowie_control_orm_repository_internal.h`
- Modify: `control/flowie_control_runtime.c`
- Modify: `control/flowie_control_config.c`
- Modify: `control/CMakeLists.txt`
- Modify: `control/tests/test_flowie_control_runtime.c`
- Modify: `control/tests/test_flowie_control_config.c`
- Create: `control/tests/test_flowie_control_orm_repository_live.c`
- Modify: `flowie/CONTROL_GUIDE.md`
- Modify: `flowie/ADR_CONTROL_STORE_PROVIDER.md`

**Interfaces:**

```c
int flowie_control_orm_repository_create(
    const flowie_control_orm_repository_config_t *config,
    flowie_control_orm_repository_provider_t **out);
const flowie_control_repository_t *flowie_control_orm_repository_view(
    const flowie_control_orm_repository_provider_t *provider);
int flowie_control_orm_repository_destroy(
    flowie_control_orm_repository_provider_t *provider, int timeout_ms);
```

- Runtime still exposes `control_store: sqlite|postgresql`; both now compose the ORM provider.
- A binary whose installed TurboDB lacks PostgreSQL support rejects PostgreSQL at configure/startup with an actionable error.
- Keep the old providers available only to differential tests until Task 9 passes.

- [ ] **Step 1: Add runtime composition and capability failure tests**

Assert SQLite/PG select the ORM provider, literal passwords are rejected, secret resolution happens before connect, missing ORM PG capability is explicit, migrate/validate is honored, and no fallback occurs after a failed connect.

- [ ] **Step 2: Run runtime/config tests and verify RED**

```powershell
ctest --preset win-dev-user -R '^test_flowie_control_config$|^test_flowie_control_runtime$|^test_flowie_control_orm_repository_live$' --output-on-failure
```

- [ ] **Step 3: Implement repository operation table and composition**

The provider owns pool/query/command objects. Destroy rejects new operations, closes the pool with the configured deadline, releases query/command objects, clears secret storage, and returns close failures rather than silently leaking.

- [ ] **Step 4: Run Control service integration tests to GREEN**

Run bootstrap, Auth, ACL, management, dashboard and HTTPS integration against SQLite, then the PG live repository. Expected: identical repository-visible behavior.

- [ ] **Step 5: Commit production composition**

```powershell
git add control/flowie_control_orm_repository.c control/flowie_control_orm_repository_internal.h control/flowie_control_runtime.c control/flowie_control_config.c control/CMakeLists.txt control/tests/test_flowie_control_runtime.c control/tests/test_flowie_control_config.c control/tests/test_flowie_control_orm_repository_live.c flowie/CONTROL_GUIDE.md flowie/ADR_CONTROL_STORE_PROVIDER.md
git commit -m "feat(control): compose ORM SQLite and PostgreSQL providers"
```

### Task 9: Differential migration, logging, and release verification

**Files:**
- Modify: `control/tests/test_flowie_control_pgsql_database_live.c`
- Create: `control/tests/test_flowie_control_orm_differential.c`
- Modify: `control/CMakeLists.txt`
- Modify: `flowie/RELEASE_GATE.md`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`
- Remove only after parity: obsolete handwritten PG query/command/provider targets from `control/CMakeLists.txt`

**Interfaces:**
- Differential harness seeds the same deterministic operations into legacy and ORM providers and compares normalized repository results and audit records.
- Logs expose only provider, schema version, pool capacity, operation class, Turbo status, and redacted SQLSTATE; no SQL parameters or secret values.

- [ ] **Step 1: Write differential and redaction tests**

Cover empty DB migration, current SQLite DB, current PG v1 schema, migrate then validate restart, CRUD/auth/ACL/policy/audit parity, connection failure, timeout, lock conflict, serialization conflict, shutdown with waiter, and log scans for known passwords/tokens/conninfo.

- [ ] **Step 2: Run differential tests and verify any mismatch is RED**

```powershell
ctest --preset win-dev-user -R '^test_flowie_control_orm_differential$' --output-on-failure
```

- [ ] **Step 3: Fix only observed parity gaps and bound diagnostics**

Place one log at the runtime/repository boundary that consumes each failure. Use counters for pool waits/timeouts and no per-query INFO logging. Keep audit records in the database transaction rather than relying on asynchronous logs.

- [ ] **Step 4: Run full local gates**

```powershell
git diff --check
cmake --fresh --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user --output-on-failure
```

Run equivalent TurboDB gates before rebuilding/installing the Flowie dependency SDK.

- [ ] **Step 5: Run Linux/EU PG release evidence**

Build Debug and Release from the same commits. Run ORM PG live, Flowie Control PG repository, migrate/validate restart, Auth/ACL management, concurrent conflicts, and log-redaction gates in run-scoped schemas. Verify PostgreSQL/Nginx and the existing standalone Flowie broker remain healthy and unchanged; remove only run-scoped schemas and test artifacts.

- [ ] **Step 6: Remove legacy production wiring and rerun all gates**

After differential parity passes, remove handwritten provider selection from production CMake/composition. Retain compatibility fixtures/tests needed to prove existing database layouts. Run the complete Task 9 matrix again.

- [ ] **Step 7: Commit final migration**

```powershell
git add control flowie/RELEASE_GATE.md flowie/LINUX_REMOTE_TEST_RUNBOOK.md
git commit -m "refactor(control): use schema-first TurboDB ORM storage"
```

### Task 10: Final cross-repository verification and handoff

**Files:**
- Verify only: `../turbodb/`
- Verify only: repository root

**Interfaces:**
- TurboDB commit(s) must be pushed/installed before the Flowie commit that requires the new ABI.
- Flowie deployment records exact TurboDB and Flowie commit IDs.

- [ ] **Step 1: Confirm clean worktrees and commit order**

```powershell
git -C ..\turbodb status --short
git status --short
git -C ..\turbodb log -5 --oneline
git log -8 --oneline
```

- [ ] **Step 2: Confirm no stale provider/build references**

Search for missing PG targets, SQLite-only rejection, legacy production provider construction, literal password acceptance, duplicate schema DDL, and old schema fingerprints. Every remaining match must be a compatibility test or documented legacy fixture.

- [ ] **Step 3: Publish evidence**

Record local Windows Debug/Release results, Linux Debug/Release results, PG version, test counts/assertions, run-scoped schema cleanup, log redaction results, container health, and the exact commits. Do not claim MQTT Broker PG persistence from the Control-only tests.
