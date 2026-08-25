# Flowie Control ORM Schema Design

## Status

Approved direction: define one backend-neutral Control schema, keep the existing
`flowie_control_repository_t` domain boundary, and implement SQLite and
PostgreSQL persistence through `TurboDB::ORM`. This migration must not introduce
dual writes, database fallback, or a direct ORM dependency in Control services.

## Goal

Make the Flowie Control Plane use one versioned logical schema and one repository
contract across SQLite and PostgreSQL. The implementation must preserve the
existing authentication, authorization, revision, audit, migration, timeout,
and shutdown behavior while removing duplicated backend query/command logic.

This design does not merge Control data with MQTT protocol state. Control uses a
`flowie_control` PostgreSQL schema namespace; protocol persistence uses its own
`flowie_protocol` namespace and lifecycle.

## Current State

Flowie already exposes a provider-neutral `flowie_control_repository_t` contract.
The SQLite implementation owns one set of prefixed tables and SQL, while the
PostgreSQL implementation owns a separate libpq connection pool, migration DDL,
typed query and command implementations, and repository adapter.

TurboDB ORM already provides SQLite and PostgreSQL drivers, explicit transactions,
parameterized queries, raw SQL, typed result access, and backend-neutral status
codes. PostgreSQL support is optional and disabled by default. Its current tests
mostly exercise a fake libpq boundary rather than a real PostgreSQL instance.
The ORM does not currently provide the complete Control connection-pool and
SQLSTATE diagnostic contract.

## Alternatives Considered

### Incremental adapter migration (selected)

Keep the Control repository interface and the existing provider selection.
Introduce a schema module and an ORM-backed adapter, prove parity against the
current SQLite and PostgreSQL contract tests, then remove the duplicated backend
implementation only after the ORM adapter passes the release gate.

This minimizes domain risk, permits old/new differential testing, and keeps each
commit reversible. It temporarily carries both implementations in the source
tree, but never runs them as dual writers.

### Immediate replacement

Replace both existing providers and migrations with ORM in one change. This has
less temporary code but makes failures in migrations, SQL dialect handling,
pooling, or domain semantics difficult to isolate. It is rejected.

### ORM only for MQTT protocol persistence

Leave Control on handwritten SQLite/libpq code. This avoids Control migration
risk but retains two persistence stacks and does not meet the requested Control
Plane unification. It remains a valid fallback if a required ORM capability
cannot meet the existing contract.

## Architecture

```text
Flowie Control services
        |
        v
flowie_control_repository_t
        |
        v
Flowie Control ORM adapter
   |                 |
   v                 v
TurboDB ORM SQLite   TurboDB ORM PostgreSQL
   |                 |
   v                 v
SQLite database      PostgreSQL flowie_control schema
```

The repository interface remains the only persistence dependency of Auth, ACL,
management, bootstrap, dashboard, and runtime composition code. ORM handles
connections, parameter binding, transactions, and result conversion below that
boundary. Flowie continues to own domain validation, optimistic revision rules,
audit semantics, schema lifecycle, configuration policy, and error translation.

## Canonical Logical Schema

The first schema version is derived from the deployed Control schema rather than
redesigning it. Existing table contents and invariants are preserved.

| Entity | Identity and purpose | Required relationships/invariants |
| --- | --- | --- |
| `schema_version` | singleton schema version and fingerprint | exactly one row after migration |
| `meta` | global Control revision | monotonically increasing revision |
| `domain` | security/tenant boundary | unique domain id |
| `user_account` | local or external principal | belongs to a domain; disabled state is durable |
| `security_group` | named group | belongs to a domain; bounded membership |
| `security_role` | named role | belongs to a domain; durable enabled state |
| `credential` | verifier metadata, salt and verifier | belongs to a user; secret material remains binary |
| `membership` | user-to-group relation | unique user/group pair; both sides must exist |
| `user_role` | user-to-role relation | unique user/role pair; both sides must exist |
| `policy_draft` | current editable policy state | domain-scoped revision |
| `acl_rule` | ordered draft/published ACL rule | stable domain/version/order identity |
| `acl_bundle` | published ACL metadata | immutable published version |
| `policy_publish_result` | idempotent publish outcome | unique request identity |
| `audit` | append-only management record | written in the same transaction as its mutation |

The schema definition records portable field types, nullability, primary keys,
foreign keys, uniqueness, indexes, and size limits. Backend-specific DDL is
limited to type spelling and safe namespace qualification. PostgreSQL uses its
`flowie_control` namespace; SQLite uses deterministic `flowie_control_` table
prefixes because SQLite has no equivalent namespace.

The schema fingerprint covers the normalized logical schema, not raw backend DDL.
SQLite and PostgreSQL therefore report the same schema version and fingerprint
for equivalent logical layouts.

## Schema Source and Generated Bindings

A repository-owned schema definition is the source of truth. TurboDB schema
validation/generation produces model metadata and a C facade used by the Control
ORM adapter. Generated files are build outputs and are not edited manually.

The schema module also exposes:

- the current integer schema version;
- the normalized fingerprint;
- backend-aware create/migrate/validate entry points;
- a read-only introspection result suitable for tests and startup diagnostics.

Migration runs only when configuration explicitly selects `schema_mode: migrate`.
Normal production startup uses `validate` and performs no DDL. Migration is
transactional and serialized per target database/schema. A failed validation or
migration is fatal; Flowie does not create an empty replacement database and does
not fall back to another provider.

## Connection and Pooling Model

The first Control ORM adapter owns a bounded pool of independent
`orm_connection_t` handles. Pool capacity and acquire timeout retain the current
configuration limits. A lease owns exactly one connection until it is returned.
Closing the provider rejects new leases, waits for outstanding leases up to the
configured deadline, and fails loudly if callers have leaked leases.

Each PostgreSQL connection is initialized with the configured statement timeout,
lock timeout, and idle-in-transaction timeout. The password remains a separate
secret value resolved from `env://...`; it is never appended to the public
conninfo string, logged, or stored after the connection has copied configuration.

Pooling initially remains a Flowie adapter concern because the Control contract
requires bounded leases and deterministic shutdown, while TurboDB ORM currently
models a single connection. A later reusable TurboDB pool can replace this local
pool without changing the repository or schema interfaces.

## Transactions and Consistency

Every mutating repository operation executes its validation reads, revision CAS,
domain changes, revision increment, and audit append in one ORM transaction.
Repository methods return success only after commit succeeds. A commit failure is
reported as indeterminate I/O unless the backend can prove rollback.

Read snapshots that compose credentials, groups, roles, and ACL bundles use the
same isolation semantics as the current PostgreSQL implementation. Pagination is
stable and deterministic through explicit ordering and exclusive cursors.

No operation may open a second connection inside an active repository transaction.
Password hashing and other expensive CPU work remains outside pool leases.

## Error and Diagnostic Contract

TurboDB ORM status codes map to existing Turbo error codes. PostgreSQL SQLSTATE
must remain available as structured diagnostic data so Flowie can preserve these
important mappings:

- serialization failure, deadlock, lock conflict and foreign-key conflict to a
  retryable/conflict status;
- unique violation to already-exists;
- statement cancellation to timeout;
- connection failures to I/O.

Errors include operation/component context but never SQL parameters, passwords,
credential verifiers, tokens, or full connection strings. Expected revision
conflicts do not produce high-volume error logs. Connection lifecycle and schema
version are INFO events; retryable failures are rate-limited WARN events; detailed
SQL diagnostics are DEBUG-only and redacted.

If TurboDB ORM cannot expose backend diagnostic codes without changing its ABI,
the ORM ABI is extended in a backward-compatible, struct-size-gated manner before
the Control adapter migration proceeds.

## Configuration and Build

The external Control configuration remains compatible:

- `storage.control_store` selects exactly one of `sqlite` or `postgresql`;
- SQLite accepts only its SQLite block;
- PostgreSQL accepts only its PostgreSQL block and requires verified TLS in
  production configurations;
- literal PostgreSQL passwords remain rejected;
- provider failure never changes the selected provider.

PostgreSQL is an optional TurboDB ORM driver component, not a compile definition
or link dependency of `TurboDB::ORM`. The core ORM package must configure, build,
install, and serve downstream consumers without finding or linking libpq. A
separate `OrmPostgreSQL::Driver` target owns libpq and exposes an explicit,
idempotent registration function. Only a Flowie Control build that enables the
PostgreSQL provider finds, links, and registers that component.

The release matrix publishes a normal TurboDB SDK and an explicit PostgreSQL
variant/component. It must not force Broker-only builds or unrelated TurboDB
downstreams to compile, install, discover, or deploy PostgreSQL. Flowie CMake
detects the optional component and either builds the Control PG adapter/tests or
reports a clear configuration error. Runtime configuration must not silently
accept PostgreSQL in a binary that did not link and register the component.

## Migration Strategy

1. Capture the existing SQLite and PostgreSQL repository behavior in a shared
   provider-parameterized contract suite.
2. Define and validate the normalized Control schema and fingerprint.
3. Split PostgreSQL into an explicitly linked ORM driver component and prove
   core-only package consumers have no PostgreSQL dependency.
4. Add real PostgreSQL live tests to TurboDB ORM for connection, parameter,
   transaction, rollback, isolation, binary values, limits, and diagnostics.
5. Implement the bounded ORM connection pool and schema lifecycle.
6. Implement the ORM-backed Control repository behind an internal build/runtime
   selection used only by tests.
7. Run differential tests against the existing providers, including seeded
   databases and migration/validate behavior.
8. Make ORM adapters the production SQLite/PostgreSQL providers after parity.
9. Remove handwritten query/command duplication in a later cleanup commit while
   retaining migration compatibility tests.

Existing production databases are validated in place. No destructive automatic
rewrite is allowed. If a backend layout differs but is logically compatible, an
explicit versioned migration must transform it transactionally.

## Verification Gates

The change is complete only when all of the following pass:

- TurboDB ORM core-only package/link test with no PostgreSQL dependency;
- TurboDB ORM PostgreSQL component unit tests;
- a real PostgreSQL live suite, not fake-libpq-only coverage;
- the shared Control repository contract against SQLite and PostgreSQL;
- migration followed by validate-only restart;
- seeded current-schema compatibility;
- revision conflict and idempotency tests;
- concurrent pool acquisition, timeout, leaked-lease shutdown and reconnect tests;
- Auth, ACL, management, bootstrap and audit integration tests;
- redaction scans over failure logs;
- Windows and Linux build/test gates;
- EU PostgreSQL validation using run-scoped schemas that are dropped afterward.

The currently deployed standalone `flowie_server` remains unaffected until its
separate protocol repository configuration is explicitly enabled.

## Non-Goals

- Sharing a transaction between Control facts and MQTT protocol state.
- Combining Control and protocol tables in one namespace.
- Dual-writing SQLite and PostgreSQL.
- Runtime fallback from PostgreSQL to SQLite or memory.
- Replacing the Repository domain boundary with direct ORM calls in services.
- Adding a generic distributed database pool to TurboDB in this migration.
