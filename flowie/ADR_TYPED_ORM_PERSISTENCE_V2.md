# Typed ORM persistence V2

## Status

Accepted. This is an intentionally incompatible standalone-storage migration.

## Decision

Standalone Flowie protocol facts use a product-owned typed repository implemented with
`TurboDB::ORM`. The repository schema contains sessions, subscriptions, inbound QoS state,
outbound deliveries, Will data, principals and retained publications. It does not contain cluster
ownership, route, binding, Raft log or snapshot tables.

One session replacement is one serializable transaction: compare the expected revision, update the
session row, replace its child rows and commit. A mismatch maps to `TURBO_EBUSY`; capacity maps to
`TURBO_ENOSPC`; invalid stored rows map to `TURBO_EPROTO`. No call falls back to an in-memory or
legacy record backend.

The schema version is V2. V1 opaque `FSES`/`FSEP` records are not decoded or migrated. Startup
rejects an unknown version and never rewrites it in place. Values passed into a call and values
supplied to a synchronous visitor are borrowed for that call only. ORM handles remain private to
the repository.

Cluster mode is excluded from this repository. TurboRaft owns both cluster data and log durability;
its state-machine snapshots and committed-log replay rebuild the in-memory cluster projections.

## Consequences

- The V1 ActiveRecord/record-store facade and binary record codecs are removed.
- `TurboDB::ORM` is the only standalone protocol repository dependency.
- Cluster and standalone persistence cannot be enabled on the same endpoint.
- Repository CAS, rollback, restart, capacity and malformed-row tests are release gates.

## Rollback

Rollback requires reinstalling the V1 binary and restoring a V1 backup. V2 never attempts a
bidirectional migration.
