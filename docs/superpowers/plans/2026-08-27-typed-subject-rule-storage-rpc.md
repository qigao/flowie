# Typed subject rule storage and RPC implementation plan

> Execute incrementally with test-driven development. Run the smallest affected TinyTest target
> after every task, then the adjacent control-plane suite, then the complete enabled CTest suite.

**Goal:** Replace ordinal/raw-text ACL management with typed domain/role/group/user storage and
structured RPC, deleting every v3 draft and published policy during upgrade.

**Architecture:** The editable source of truth is one canonical ACL document per typed subject,
indexed by `(domain_id, subject_kind, subject_id)` and ordered by a domain-unique ordinal. RPC JSON
is converted to/from the existing ACL document type only in the adapter. Publish compiles typed
draft documents into the existing runtime bundle interface. Schema v4 performs a destructive,
policy-only v3 transition.

**Tech stack:** C11, SQLite, PostgreSQL/libpq, TurboParser JSON, Iris JSON-RPC, TinyTest, CMake
presets.

---

### Task 1: Define the typed domain port and SQLite contract

**Files:**
- Modify: `control/flowie_control_store_internal.h`
- Modify: `control/flowie_control_repository_internal.h`
- Modify: `control/flowie_control_management_service_internal.h`
- Modify: `control/flowie_control_management_service.c`
- Test: `control/tests/test_flowie_control_store.c`
- Test: `control/tests/flowie_control_management_repository_contract.h`

1. Add failing tests for typed put/get/list/delete, subject-key replacement, domain-ordinal
   uniqueness, and lookup without scanning unrelated users.
2. Replace legacy rule command/view and repository operations with subject-rule types and methods.
3. Make the management service enforce existing viewer/policy-admin permissions on the typed port.
4. Compile and run the focused tests until green.

### Task 2: Implement SQLite v4 storage and destructive upgrade

**Files:**
- Modify: `control/flowie_control_store.c`
- Modify: `control/flowie_control_repository.c`
- Test: `control/tests/test_flowie_control_store.c`
- Test: `control/tests/flowie_control_auth_repository_contract.h`

1. Add a failing fixture that creates v3 draft/published data, opens it with v4, and proves all
   policy data is gone while domain/identity data remains.
2. Add schema-v4 policy tables and the transactional v3 policy purge/version transition.
3. Implement typed key validation, direct get, filtered keyset list, put, and delete.
4. Update validation/publish/subject-reference checks to use typed columns and canonical documents.
5. Run the SQLite store, auth repository, and management repository tests.

### Task 3: Replace the public management RPC

**Files:**
- Modify: `control/flowie_control_management_rpc.c`
- Test: `control/tests/test_flowie_control_management_rpc.c`

1. Add failing RPC round-trip tests for structured put/get/list/delete.
2. Add failing tests proving all three `control.policy.rule.*` methods are method-not-found and raw
   `rule_line` is rejected.
3. Implement bounded JSON parsing/serialization for subject kind, connection, and entries.
4. Register only the four `control.policy.subject_rule.*` methods.
5. Run the management RPC tests.

### Task 4: Bring PostgreSQL to schema and port parity

**Files:**
- Modify: `control/flowie_control_pgsql_database.c`
- Modify: `control/flowie_control_pgsql_command.c`
- Modify: `control/flowie_control_pgsql_command_internal.h`
- Modify: `control/flowie_control_pgsql_query.c`
- Modify: `control/flowie_control_pgsql_query_internal.h`
- Modify: `control/flowie_control_pgsql_repository.c`
- Test: `control/tests/test_flowie_control_pgsql_database.c`
- Test: `control/tests/test_flowie_control_pgsql_database_live.c`

1. Update live/contract tests for destructive v3 policy purge and typed operations.
2. Implement schema-v4 migration under the advisory lock.
3. Replace ordinal SQL with typed subject key SQL and filtered list/get queries.
4. Run local compile/syntax checks; run live tests when `TURBO_FLOW_PGSQL_TEST_CONNINFO` exists.

### Task 5: Update dashboard, documentation, and compatibility checks

**Files:**
- Modify: `control/flowie_control_dashboard.c`
- Modify: `control/flowie_control_dashboard_view.c`
- Modify: `control/assets/control.js`
- Modify: `control/tests/test_flowie_control_dashboard.c`
- Modify: `flowie/MANAGEMENT_RPC_API.md`
- Modify: `flowie/ACL_GRAMMAR.md`
- Modify: `flowie/THIRD_PARTY_INTEGRATION.md`
- Modify: `flowie/ADR_SUBJECT_SCOPED_MQTT_ACL.md`

1. Change dashboard tests/forms to typed subject operations and fields.
2. Remove every documented legacy method/example and document destructive upgrade behavior.
3. Search the repository for remaining production/documentation references to legacy methods and
   old draft lookup semantics.
4. Run JavaScript syntax checks and dashboard tests.

### Task 6: Full verification and PR update

1. Build the affected targets with the repository MSVC preset.
2. Run all enabled CTest tests; record any unavailable PostgreSQL live test explicitly.
3. Inspect `git diff --check`, status, and the final diff for unrelated changes.
4. Commit and push the completed change to the existing pull request branch.

