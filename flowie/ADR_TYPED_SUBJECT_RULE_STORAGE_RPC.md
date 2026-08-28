# ADR: Typed subject ACL storage and management RPC

## Status

Accepted for the original v5 control-plane schema. The current v6 schema preserves this typed
subject storage and RPC contract, widens all persisted 64-bit values to `BIGINT`, and rejects v5
without migration. This remains an intentionally incompatible boundary.

## Context

The control plane must make the policy subject an explicit key so shared role/group rules do not
devolve into one persisted rule per device. No legacy raw-rule storage or RPC surface is retained.

## Decision

- A domain is the policy scope, not an ACL subject.
- Valid subjects are exactly `role`, `group`, and `user`.
- Draft rules are keyed by `(domain_id, subject_kind, subject_id)`.
- `ordinal` remains a unique, stable ordering field within a domain for deterministic compilation
  and display; it is not a lookup or delete key.
- The canonical ACL document remains the single persisted representation of connection and topic
  permissions. `subject_kind` and `subject_id` are validated index columns derived from that
  document at the write boundary and checked again when read.
- Management RPC uses structured methods:
  - `control.policy.subject_rule.put`
  - `control.policy.subject_rule.get`
  - `control.policy.subject_rule.list`
  - `control.policy.subject_rule.delete`
- Put accepts `subject_kind`, `subject_id`, `ordinal`, `connection`, and an `entries` array. Each
  entry has `effect`, `access`, and `topic`. The RPC adapter creates the canonical document; callers
  cannot submit raw grammar text.
- Get/delete address a rule by `subject_kind` and `subject_id`. List may filter by subject kind and
  pages by `ordinal`.
- The old `control.policy.rule.*` methods are not registered and return JSON-RPC method-not-found.

## Schema boundary

Fresh databases create only the v6 subject-keyed draft and published tables. Earlier control-plane
schemas, including v5, are rejected during startup; Flowie contains no migration, translation,
fallback, or legacy table reader. Operators must explicitly create a fresh v6 store and submit
structured subject rules.
The empty policy state remains fail closed until an administrator publishes it.

## State and failure semantics

The draft table is the source of truth for editable policy. Published compiled rules are a derived,
atomically replaced snapshot. A put/delete validates identity references and optimistic revision
before committing the draft and its audit record in one transaction. Publish validates and compiles
all current drafts before replacing the runtime snapshot.

Any malformed structured input, subject/document mismatch, missing or disabled subject, conflicting
domain ordinal, or schema validation failure aborts the operation. There is no legacy fallback.

## Compatibility and verification

This changes the management API without a compatibility path. Operators must create a v6 store,
then submit and publish structured rules. Verification covers:

- rejection of non-v6 SQLite and PostgreSQL schemas;
- typed put/get/list/delete and subject/ordinal uniqueness;
- structured RPC round trips and rejection of legacy methods/raw documents;
- publish, runtime bundle loading, and fail-closed empty-policy behavior;
- dashboard and API documentation using only the new methods.
