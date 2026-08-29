# Flowie MQTT cluster ownership

## Status

Accepted.

## Decision

Cluster mode uses TurboRaft as the only durable authority for both replicated data and the Raft
log. Flowie does not open a TurboDB ORM repository, PostgreSQL fact store, Redis route store, or
FlowStore backend in cluster mode.

The cluster boundary is split as follows:

- `TurboRaft::Service` owns consensus, ordering, commit and application of log entries.
- `TurboRaft::WalStorage` owns the local Raft log, term/vote metadata and Raft snapshots.
- `TurboRaft::DataStream` and `TurboRaft::FlowMQ` move bounded replicated payloads and Raft peer
  traffic. A payload becomes durable only after its descriptor is committed through Raft.
- `flowie_cluster_state_machine` applies committed owner and publish entries.
- `flowie_cluster_owner_directory` is a rebuildable in-memory projection. Its revision is the
  committed Raft log index; it is not an independent fact source.
- State-machine snapshot creation and restoration use the snapshot callbacks configured on the
  Raft store. Snapshot bytes are stored by TurboRaft together with the log boundary.

Standalone mode is separate. Its MQTT session, subscription, inflight, delivery, Will and retained
facts use `Orm::C` through `flowie_protocol_repository`. Standalone ORM state is never opened
or reused by cluster mode.

## Invariants

1. A cluster mutation is externally visible only after the corresponding Raft entry is committed
   and applied.
2. Cluster state has one durable source: TurboRaft log plus its state-machine snapshot.
3. In-memory owner and routing projections are derived only from committed entries or snapshot
   restoration.
4. Recovery restores the latest TurboRaft snapshot and replays the remaining committed log in
   order; it never reconciles against an ORM, PostgreSQL or Redis database.
5. Missing, corrupt or unsupported snapshot/log data fails startup. There is no fallback to
   standalone storage.
6. Cluster and standalone persistence bindings are mutually exclusive at the endpoint boundary.

## Lifecycle

Creation order is owner projection, state machine, TurboRaft runtime, then DataStream router.
Startup begins peer service before accepting cluster work. Shutdown stops endpoint admission,
stops the TurboRaft generation, closes WalStorage, and finally destroys projections. A failed
stop or close is reported and does not silently switch to standalone mode.

## Verification

The cluster test set covers SQLite log recovery, snapshot restore, committed owner projection,
state-machine dispatch, FlowMQ peer transport, DataStream quorum transfer and generation lifecycle.
Standalone repository tests cover only the MQTT V2 ORM schema.
