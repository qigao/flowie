# TurboRaft + FlowMQ cluster runtime

## Scope

Flowie cluster mode uses TurboRaft as the single durable owner for replicated data and the Raft
log. FlowMQ transports peer messages; TurboRaft DataStream transfers bounded payload data. Neither
component introduces a second ORM, Redis, PostgreSQL or FlowStore fact source.

## Runtime path

```text
client mutation
  -> Flowie cluster proposal
  -> TurboRaft::Service log replication
  -> committed entry
  -> Flowie state machine
  -> in-memory owner/publish projections

large publish payload
  -> TurboRaft::DataStream quorum transfer
  -> committed descriptor in the Raft log
  -> state-machine apply callback
```

`TurboRaft::FlowMQ` carries Raft peer messages. `TurboRaft::WalStorage` stores term/vote, log
entries and snapshots. Snapshot callbacks serialize and restore the complete state-machine data at
the committed snapshot index. Recovery never consults another database.

## Durability contract

- Accepted transport bytes are not durable by themselves.
- Data is durable only when the DataStream durability condition is met and its descriptor is
  committed through Raft.
- A mutation becomes visible only from the state-machine apply callback.
- Snapshot storage and remaining-log replay reconstruct the same state in index order.
- Corrupt or unsupported data fails fast; there is no backend fallback.

## Bounds

Peer queues, inbound/outbound streams, event bytes, log entries and snapshot bytes all have explicit
configuration limits. Capacity exhaustion is returned to the caller and cannot be converted into a
best-effort local enqueue.

## Verification

Relevant CTest targets cover Raft SQLite recovery, snapshot restoration, FlowMQ peer runtime,
DataStream ingress/egress/quorum behavior, owner projection, state-machine dispatch and generation
lifecycle.
