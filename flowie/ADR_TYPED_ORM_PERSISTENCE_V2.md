# Typed ORM persistence V2

## Status

Accepted. This is an intentionally incompatible standalone-storage migration.

## Decision

Standalone Flowie protocol facts use a product-owned typed repository implemented with
`Orm::C`. The repository schema contains sessions, subscriptions, inbound QoS state,
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

Queries use the TurboDB typed CFlow API. Each query declares an explicit CMeta row shape and
consumes its `cflow_source` synchronously; `VALUE` storage is destroyed immediately after its
visitor returns. A `WAIT` result is rejected because the standalone repository is a synchronous
adapter. Command Sources must yield exactly one `VALUE_AND_DONE`. Source destruction always
precedes query, transaction and connection destruction. The connection cursor budget reserves one
row beyond the largest configured business limit, so the adapter observes an N+1 row and preserves
the `TURBO_ENOSPC` capacity result instead of collapsing it into a datastore error.

Retained rows and per-session child rows are streamed. Session headers are the only buffered view:
they are accumulated in a TurboSTL vector bounded by `max_sessions`, then their Source is closed
before child queries start. This preserves PostgreSQL's single-active-result ordering without
allocating capacity for absent sessions or issuing one top-level session query per device.

Cluster mode is excluded from this repository. TurboRaft owns both cluster data and log durability;
its state-machine snapshots and committed-log replay rebuild the in-memory cluster projections.

## Consequences

- The V1 ActiveRecord/record-store facade and binary record codecs are removed.
- `Orm::C` is the only standalone protocol repository dependency.
- Cluster and standalone persistence cannot be enabled on the same endpoint.
- Repository CAS, rollback, restart, capacity and malformed-row tests are release gates.

## Rollback

Rollback requires reinstalling the V1 binary and restoring a V1 backup. V2 never attempts a
bidirectional migration.
