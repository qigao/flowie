# Control Store Schema Generation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the two handwritten Control store DDL strings with SQLite and PostgreSQL DDL generated from one versioned TBE schema without changing the persisted schema contract.

**Architecture:** TurboParser's database IR gains an explicit `db_foreign_key_on_delete(name, action)` annotation so the schema can retain both existing cascade constraints. Flowie owns one `flowie_control_store.schema`, generates both dialects at build time, embeds them, selects the dialect from the configured TurboDB driver, and continues applying statements through the existing bounded database adapter.

**Tech Stack:** C11, TBE `tbe_compiler`, Mustache DDL templates, TurboDB ORM, CMake presets, TinyTest.

**Spec:** `flowie/CONTROL_GUIDE.md`

## Global Constraints

- Preserve schema version `7` and fingerprint `flowie-control-persistent-session-schema-v7-20260829`.
- Preserve all 16 tables, three management-session indexes, seed rows, CHECK constraints, foreign keys, and the two `ON DELETE CASCADE` actions.
- Keep `tbe_compiler` build-time only and discover it exclusively from `TURBOPARSER_ROOT/bin`.
- Preserve legacy-schema rejection and repeat-open behavior.
- Do not commit `.codegraph/` or generated build-tree files.

---

### Task 1: TBE foreign-key delete actions

**Files:**
- Modify: `C:/projects/cpp/turbonet/turbo-parser/tbe/tbe_compiler/database_schema.c`
- Modify: `C:/projects/cpp/turbonet/turbo-parser/tbe/tbe_compiler/templates/sqlite_schema.mustache`
- Modify: `C:/projects/cpp/turbonet/turbo-parser/tbe/tbe_compiler/templates/postgresql_schema.mustache`
- Modify: `C:/projects/cpp/turbonet/turbo-parser/tbe/tbe_compiler/test_tbe_compiler.c`
- Modify: `C:/projects/cpp/turbonet/turbo-parser/tbe/tbe_compiler/CLI_OPTIONS.md`

**Interfaces:**
- Consumes: existing `db_foreign_key(name, Target, local, remote, ...)` annotations.
- Produces: `db_foreign_key_on_delete(name, cascade|restrict|no_action)` and `sql_on_delete_action` in the normalized foreign-key IR.

- [ ] **Step 1: Add failing compiler tests**

Add a fixture with `db_foreign_key_on_delete(fk_child_parent, cascade)` and assert both dialect outputs contain `ON DELETE CASCADE`; add invalid fixtures for a missing constraint, duplicate action, unsupported action, and wrong argument count.

- [ ] **Step 2: Run the focused compiler test and verify RED**

Run `ctest --preset win-release-user -R test_tbe_compiler --output-on-failure`; expect rejection of the new annotation before implementation.

- [ ] **Step 3: Implement normalized action validation and rendering**

Allow the annotation only at message scope, resolve its named foreign key after all keys are built, store one normalized uppercase SQL token, and append ` ON DELETE {{sql_on_delete_action}}` only when present.

- [ ] **Step 4: Run the focused compiler test and verify GREEN**

Run `ctest --preset win-release-user -R test_tbe_compiler --output-on-failure`; expect all compiler tests to pass.

### Task 2: Declarative Flowie Control store schema

**Files:**
- Create: `control/schema/flowie_control_store.schema`
- Create: `control/flowie_control_store_schema_internal.h`
- Create: `control/tests/test_flowie_control_store_schema.c`
- Modify: `cmake/EmbedGeneratedText.cmake`
- Modify: `control/CMakeLists.txt`

**Interfaces:**
- Consumes: `tbe_compiler --lang sqlite|postgresql` and the new delete-action annotation.
- Produces: `flowie_control_store_schema_sql(driver, size_out)`, generated `flowie-control-store.sqlite.sql`, and generated `flowie-control-store.postgresql.sql`.

- [ ] **Step 1: Add a failing behavior test**

Create a TinyTest that applies the embedded SQLite DDL to an empty database, asserts 16 Control tables, three named indexes, schema version/fingerprint and seed rows, then verifies deleting a parent removes a management session and published rules through real foreign keys.

- [ ] **Step 2: Run the schema test and verify RED**

Build and run `test_flowie_control_store_schema`; expect link or compile failure because the generated store-schema API does not exist.

- [ ] **Step 3: Add the TBE schema and build generation**

Model every current table and constraint in `flowie_control_store.schema`, generate both dialect files, embed both, add a generation target, install the DDL files, and make the generated API reject unknown drivers.

- [ ] **Step 4: Run the schema test and verify GREEN**

Build and run `test_flowie_control_store_schema`; expect all catalog, seed, constraint, and cascade assertions to pass.

### Task 3: Store initialization cutover

**Files:**
- Modify: `control/flowie_control_store.c`
- Modify: `control/tests/test_flowie_control_store.c`

**Interfaces:**
- Consumes: `flowie_control_store_schema_sql(driver, size_out)`.
- Produces: unchanged `flowie_control_store_open()` behavior backed by generated DDL.

- [ ] **Step 1: Add a repeat-open regression assertion**

Extend the store test to open, close, and reopen the same SQLite database and verify revision and schema fingerprint remain unchanged.

- [ ] **Step 2: Run the store test and verify the new assertion**

Run `ctest --preset win-release-user -R test_flowie_control_store --output-on-failure` before removing the literals to characterize existing behavior.

- [ ] **Step 3: Remove both handwritten DDL strings**

Select generated SQLite/PostgreSQL DDL from the copied TurboDB driver, apply it at the same initialization boundary, retain preflight and fingerprint validation, and keep failures mapped through the existing database status adapter.

- [ ] **Step 4: Run focused and adjacent regression tests**

Run the two schema tests, `test_flowie_control_store`, `test_flowie_control_repository`, and `test_flowie_control_data_transfer` with `--output-on-failure`.

### Task 4: Package verification

**Files:**
- Verify: generated and installed artifacts only.

**Interfaces:**
- Consumes: repository CMake user presets.
- Produces: reproducible build/test/install evidence.

- [ ] **Step 1: Run the full Release build and Control tests**

Run `cmake --build --preset win-release-user` and `ctest --preset win-release-user -L control-plane --output-on-failure`.

- [ ] **Step 2: Run full CTest and install**

Run `ctest --preset win-release-user --output-on-failure` and install through the repository install preset or an isolated staging prefix.

- [ ] **Step 3: Verify artifacts and source hygiene**

Confirm both store DDL files and both data-manifest DDL files are installed, run `git diff --check`, and confirm no generated build output or `.codegraph/` is staged.
