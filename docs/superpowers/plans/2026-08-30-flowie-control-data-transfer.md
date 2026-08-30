# Flowie Control Data Transfer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a versioned, portable SQLite Domain manifest and a `flowie-control-data` CLI that exports one Control Domain and imports it through audited domain commands.

**Architecture:** `tbe_compiler` runs only during the Flowie build to compile `flowie_control_data.schema` into SQLite and PostgreSQL DDL. The CLI embeds the generated SQLite DDL, exports a consistent read-only Repository snapshot into a new manifest database, validates an imported manifest in a trusted in-memory database, and applies it through `flowie_control_management_*` commands. Credentials, password verifiers, generated tokens, management sessions, and historical audit rows never enter the manifest.

**Tech Stack:** C11, CMake presets, TurboParser `tbe_compiler`, TurboDB ORM C API, Flowie Control Repository/Management Service, TurboUtils TinyTest.

**Spec:** `flowie/ADR_AUTH_DOMAIN_SERVICE.md`

## Global Constraints

- The manifest contains exactly one non-`system` Domain and has an explicit format version.
- Export is read-only and succeeds only when the Repository revision is unchanged from the first read through the final read.
- Export refuses to overwrite an existing path and publishes output through a same-directory temporary file plus atomic rename.
- Import validates the complete source before the first target mutation.
- Import uses idempotent, deterministic request IDs and the current Repository revision for every command.
- Import order is Domain, users, Groups by depth, Roles, memberships, Role assignments, ACL drafts, optional publish.
- Import never writes Control tables directly.
- Credentials, password verifiers, service tokens, management sessions, and historical audit rows are excluded.
- `--dry-run` performs source and target preflight without changing the target Repository.
- `tbe_compiler` is discovered only under `$ENV{TURBOPARSER_ROOT}/bin` and is never a runtime dependency.
- No existing Control runtime, RPC, Dashboard, or store-visible behavior changes.

---

### Task 1: Build-time manifest schema generation

**Files:**
- Create: `control/schema/flowie_control_data.schema`
- Create: `cmake/EmbedGeneratedText.cmake`
- Create: `control/flowie_control_data_schema_internal.h`
- Modify: `control/CMakeLists.txt`
- Test: `control/tests/test_flowie_control_data_schema.c`

**Interfaces:**
- Produces: `const char *flowie_control_data_schema_sql(size_t *size_out)` returning the embedded generated SQLite DDL.
- Produces: build artifacts `flowie-control-data.sqlite.sql` and `flowie-control-data.postgresql.sql`.
- Consumes: `$ENV{TURBOPARSER_ROOT}/bin/tbe_compiler`.

- [ ] **Step 1: Write the failing schema behavior test**

  Add a TinyTest spec that executes `flowie_control_data_schema_sql()` against an in-memory SQLite connection and asserts that the metadata initializer, Domain row table, user table, Group table, membership table, Role table, assignment table, policy-rule table, policy-status table, indexes, checks, and foreign keys are usable.

- [ ] **Step 2: Run the test target and confirm RED**

  Run `cmake --build --preset win-release-user --target test_flowie_control_data_schema` from the VS developer environment. Expected failure: the target/source and `flowie_control_data_schema_sql` do not exist.

- [ ] **Step 3: Add the versioned TBE schema**

  Define schema `FlowieControlData` version `1` with these archive tables and keys:

  ```text
  metadata(singleton PK, format_version, domain_id, source_revision)
  users(principal_id PK, principal_type, enabled)
  groups(group_id PK, optional parent_group_id, depth)
  memberships(principal_id + group_id composite PK)
  roles(role_id PK, enabled)
  user_roles(principal_id + role_id composite PK)
  policy_rules(subject_kind + subject_id composite PK, ordinal unique index, rule_document)
  policy_status(singleton PK, published, expires_at)
  ```

  Add same-schema foreign keys, identifier/boolean/range checks, deterministic indexes, and a `db_init(sqlite, ...)` metadata format-version initializer. Do not model credentials, sessions, or audit records.

- [ ] **Step 4: Add strict CMake generation**

  Find `tbe_compiler` only in `$ENV{TURBOPARSER_ROOT}/bin`, verify that `--lang sqlite` is supported, generate both dialect DDL files, convert the SQLite SQL bytes into one generated C array using `EmbedGeneratedText.cmake`, and install the two SQL artifacts under `share/turboflow/control`.

- [ ] **Step 5: Run the focused test and confirm GREEN**

  Reconfigure with `cmake --fresh --preset win-release-user`, build `test_flowie_control_data_schema`, then run `ctest --preset win-release-user -R test_flowie_control_data_schema --output-on-failure`.

### Task 2: Direct membership and Role-assignment query views

**Files:**
- Modify: `control/flowie_control_store_internal.h`
- Modify: `control/flowie_control_store.c`
- Modify: `control/flowie_control_repository_internal.h`
- Modify: `control/flowie_control_repository.c`
- Modify: `control/tests/flowie_control_repository_contract.h`
- Modify: `control/tests/test_flowie_control_store.c`

**Interfaces:**
- Produces: `flowie_control_membership_view_t` and `flowie_control_user_role_view_t`.
- Produces: keyset-paginated `membership_list` and `assignment_list` Repository operations using `(principal_id, group_id)` and `(principal_id, role_id)` cursors.

- [ ] **Step 1: Add failing Repository contract tests**

  Create two users, nested Groups, two direct memberships, two Roles, and two direct assignments. Assert that paginated direct lists return only persisted edges in lexical tuple order and do not synthesize inherited Groups or Roles.

- [ ] **Step 2: Run the Repository tests and confirm RED**

  Build `test_flowie_control_repository` and run its direct-edge filter. Expected failure: the Repository operations and view types are absent.

- [ ] **Step 3: Implement bounded tuple-keyset queries**

  Add store queries ordered by both key columns, validate both cursor components, decode bounded text columns into fixed-size views, return at most `FLOWIE_CONTROL_PAGE_MAX` rows, and preserve the existing `count_out`/`has_more_out` failure semantics.

- [ ] **Step 4: Bind and validate Repository version 6**

  Add the operations to the Group/Role operation tables, bump `FLOWIE_CONTROL_REPOSITORY_VERSION` to `6`, require non-null implementations in `flowie_control_repository_validate`, and update fake/contract Repository initializers.

- [ ] **Step 5: Run store and Repository tests and confirm GREEN**

  Run `ctest --preset win-release-user -R "test_flowie_control_(store|repository)" --output-on-failure`.

### Task 3: Archive creation, validation, and deterministic access

**Files:**
- Create: `control/flowie_control_data_archive_internal.h`
- Create: `control/flowie_control_data_archive.c`
- Test: `control/tests/test_flowie_control_data_archive.c`
- Modify: `control/CMakeLists.txt`

**Interfaces:**
- Produces: opaque `flowie_control_data_archive_t`.
- Produces: `flowie_control_data_archive_create`, `_open_validated`, `_destroy`, row append/read helpers, and `_publish`.
- Ownership: archive owns its ORM connection and temporary path; callers own borrowed row views only for the callback duration.

- [ ] **Step 1: Add failing archive tests**

  Cover new-file creation, refusal to overwrite, incomplete-export cleanup, exact metadata values, deterministic row iteration, malformed/missing tables, wrong format version, broken references, duplicate rows, invalid canonical ACL text, and atomic publication.

- [ ] **Step 2: Run the archive test and confirm RED**

  Build and run `test_flowie_control_data_archive`. Expected failure: archive API is absent.

- [ ] **Step 3: Implement the archive adapter**

  Use a private SQLite TurboDB configuration with `read_write_create` for new manifests and `read_only` for sources. Execute only embedded generated DDL for creation. Bind every value through prepared statements, keep a transaction open until publication, and cap every table at named manifest limits.

- [ ] **Step 4: Validate through a trusted reconstruction**

  Open the untrusted source read-only, copy every row through typed/bounded decoders into a fresh in-memory database created from embedded DDL, require exactly one metadata row and one policy-status row, run SQLite integrity/foreign-key checks, parse every ACL document canonically, and return only the trusted in-memory archive to import.

- [ ] **Step 5: Run the archive test and confirm GREEN**

  Run `ctest --preset win-release-user -R test_flowie_control_data_archive --output-on-failure`.

### Task 4: Consistent Domain export and audited import

**Files:**
- Create: `control/flowie_control_data_transfer_internal.h`
- Create: `control/flowie_control_data_transfer.c`
- Test: `control/tests/test_flowie_control_data_transfer.c`
- Modify: `control/CMakeLists.txt`

**Interfaces:**
- Produces: `flowie_control_data_export(repository, domain_id, output_path, result)`.
- Produces: `flowie_control_data_import(repository, input_path, dry_run, result)`.
- Result reports source revision, final target revision, entity counts, policy publication status, and whether any target mutation occurred.

- [ ] **Step 1: Add a failing export behavior test**

  Build a real in-memory Control store containing users, nested Groups, direct memberships, Roles, direct assignments, canonical ACL drafts, and a published policy. Export and query the resulting manifest database. Assert exact declarative rows and assert that credentials, sessions, and audit tables do not exist.

- [ ] **Step 2: Confirm export RED and implement minimal export**

  Read start revision, page through Domain-owned query views, append rows in deterministic order, read policy state/rules, read final revision, reject a changed revision, then publish the archive atomically.

- [ ] **Step 3: Add failing dry-run/import tests**

  Assert that dry-run validates without changing revision. Import the same manifest into a fresh bootstrapped store, assert equivalent users, hierarchy, direct edges, Roles, ACL drafts and publication state, assert credentials are absent, and assert replay after an interrupted prefix completes idempotently.

- [ ] **Step 4: Confirm import RED and implement command replay**

  Preflight the validated archive and target Domain, create a trusted system-admin caller, obtain the current revision before each command, use deterministic `flowie-data-v1-*` request IDs, apply commands in dependency order, validate policy before optional publish, and stop immediately on the first command error. A partial import remains replayable; no compensating direct table writes are allowed.

- [ ] **Step 5: Run the transfer test and adjacent security tests**

  Run `ctest --preset win-release-user -R "test_flowie_control_(data_transfer|management_service|bootstrap)" --output-on-failure`.

### Task 5: Operator CLI and shared database configuration resolution

**Files:**
- Create: `control/flowie_control_database_config_internal.h`
- Create: `control/flowie_control_database_config.c`
- Create: `control/flowie_control_data_options_internal.h`
- Create: `control/flowie_control_data_options.c`
- Create: `control/flowie_control_data_main.c`
- Modify: `control/flowie_control_runtime.c`
- Modify: `control/CMakeLists.txt`
- Test: `control/tests/test_flowie_control_data_options.c`
- Test: `control/tests/test_flowie_control_data_cli.c`

**Interfaces:**
- CLI: `flowie-control-data export --config <yml> --domain <id> --output <manifest.db>`.
- CLI: `flowie-control-data import --config <yml> --input <manifest.db> [--dry-run]`.
- Produces shared `flowie_control_database_config_resolve(config, orm_config, options)` used by both Control runtime and data CLI.

- [ ] **Step 1: Add failing option-parser tests**

  Cover both subcommands, required/duplicate/unknown options, environment-backed config selection, path bounds, `--dry-run` only on import, and rejection of `system` export/import.

- [ ] **Step 2: Confirm parser RED and implement options**

  Use the existing TurboUtils command parser pattern and fixed-size owned option buffers. Return Turbo error codes; print user-facing diagnostics only in `main`.

- [ ] **Step 3: Add failing CLI integration tests**

  Spawn the real executable against temporary SQLite Control databases, verify export/import/dry-run exit codes and results, verify refusal to overwrite, and verify malformed manifests fail before target revision changes.

- [ ] **Step 4: Refactor database configuration once**

  Move the current runtime-only TurboDB option/`env://` secret resolution into `flowie_control_database_config.c`; preserve the exact current validation and secret lifetime semantics; link it into runtime and CLI without duplicating configuration logic.

- [ ] **Step 5: Implement and install the CLI**

  Load the existing Control YAML, resolve the exact TurboDB target, open the store, call transfer APIs, emit one concise success/error summary, install `flowie-control-data` under `bin`, and enable BoringSSL linkage only if its linked dependencies require it.

- [ ] **Step 6: Run parser and CLI tests**

  Run `ctest --preset win-release-user -R "test_flowie_control_data_(options|cli)" --output-on-failure`.

### Task 6: Documentation and full verification

**Files:**
- Modify: `flowie/CONTROL_GUIDE.md`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

**Interfaces:**
- Documents the manifest contract, offline-writer precondition, security exclusions, commands, dry-run, replay after interruption, and rollback via the separately captured physical backup.

- [ ] **Step 1: Document the exact workflow**

  Add executable examples for export, import dry-run, import, verification, and failure recovery. State that a manifest is not a credential/session/physical backup and that Control must be stopped before target mutation.

- [ ] **Step 2: Verify generated artifacts**

  Build the two DDL targets, apply SQLite DDL to a temporary database, and parse PostgreSQL DDL with the live PostgreSQL gate when `FLOWIE_TURBODB_TEST_CONNINFO` is available.

- [ ] **Step 3: Run focused then adjacent then full tests**

  Run all new data tests, all `flowie;control-plane;storage-contract` tests, then `ctest --preset win-release-user --output-on-failure`.

- [ ] **Step 4: Install and inspect package output**

  Run `cmake --build --preset install-win-release-user`, verify `bin/flowie-control-data.exe` and both installed DDL files, and execute the installed CLI help plus one temporary SQLite round trip.

- [ ] **Step 5: Review repository state**

  Confirm `.codegraph/`, build outputs, manifests, temporary databases, and generated intermediate C files are not tracked. Review `git diff --check`, `git status --short`, and the plan requirements before reporting completion. Do not commit or push unless the user asks.
