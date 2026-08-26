# ADR: Typed subject ACL storage and management RPC

## Status

Accepted for the v4 control-plane schema. This is an intentionally incompatible change.

## Context

The v3 control plane stores draft ACL documents by `(domain_id, ordinal)` and exposes raw
`control.policy.rule.put/list/delete` methods. The subject is embedded in `rule_line`, so storage
and query code must parse every document to discover whether it belongs to a role, group, or user.
That model also encourages creating one rule per device instead of assigning shared role/group
rules.

The v4 design must make the policy subject an explicit key and must not retain or translate any v3
draft or published policy data.

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

## Destructive schema transition

On v3 to v4 upgrade, one transaction drops all v3 policy drafts, published bundles, compiled rules,
and publish idempotency results. No rule is parsed, copied, or retained. Identity, group, role,
membership, credential, audit, and domain data remain intact. The new empty policy state is fail
closed until an administrator submits and publishes v4 subject rules.

Fresh databases create only the v4 draft schema. Published compiled tables keep the runtime schema
contract but start empty after upgrade.

PostgreSQL performs the same destructive policy-only transition while holding the existing schema
migration advisory lock. SQLite performs it in an immediate transaction before v4 schema
validation.

## State and failure semantics

The draft table is the source of truth for editable policy. Published compiled rules are a derived,
atomically replaced snapshot. A put/delete validates identity references and optimistic revision
before committing the draft and its audit record in one transaction. Publish validates and compiles
all current drafts before replacing the runtime snapshot.

Any malformed structured input, subject/document mismatch, missing or disabled subject, conflicting
domain ordinal, or schema migration failure aborts the operation. There is no legacy fallback.

## Compatibility and verification

This changes the management API and deletes persisted ACL data. Operators must submit and publish
new v4 rules after upgrade. Verification covers:

- destructive v3→v4 SQLite and PostgreSQL schema transitions;
- typed put/get/list/delete and subject/ordinal uniqueness;
- structured RPC round trips and rejection of legacy methods/raw documents;
- publish, runtime bundle loading, and fail-closed empty-policy behavior;
- dashboard and API documentation using only the new methods.
