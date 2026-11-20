# Flowie MQTT server/cluster ownership

## Status

Accepted.

## Decision

Cluster mode uses TurboRaft as the only durable authority for both replicated data and the Raft
log. Flowie does not open a TurboDB ORM repository, PostgreSQL fact store, Redis route store, or
FlowStore backend in server/cluster mode.

The server/cluster boundary is split as follows:

- `TurboRaft::Service` owns consensus, ordering, commit and application of log entries.
- `TurboRaft::WalStorage` owns the local Raft log, term/vote metadata and Raft snapshots.
- `TurboRaft::DataStream` and `TurboRaft::CNet` move bounded replicated payloads and Raft peer
  traffic. A payload becomes durable only after its descriptor is committed through Raft.
- `flowie_cluster_state_machine` applies committed owner and publish entries.
- `flowie_cluster_owner_directory` is a rebuildable in-memory projection. Its revision is the
  committed Raft log index; it is not an independent fact source.
- State-machine snapshot creation and restoration use the snapshot callbacks configured on the
  Raft store. Snapshot bytes are stored by TurboRaft together with the log boundary.

Standalone mode is separate. Its MQTT session, subscription, inflight, delivery, Will and retained
facts use `Orm::C` through `flowie_protocol_repository`. Standalone ORM state is never opened
or reused by server/cluster mode.

## Invariants

1. A server/cluster mutation is externally visible only after the corresponding Raft entry is committed
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

### TurboRaft 0.3.2 peer ownership

The runtime owns WalStorage and one `tr_raft_cnet_peer_t` for each remote voter. It borrows
the caller's CNet clients; every client and every runtime operation has the same fixed progress
owner. The host owns connection establishment, TLS identity verification, completed TurboRaft
handshake negotiation and CNet polling. After creation, the host obtains each peer observer
from the runtime and supplies it to CNet connect/accept. No second progress loop or networking
service is created inside Flowie.

The voter list is the topology authority. There must be exactly one configured peer for each
remote voter, no duplicate client, and at most `TR_RAFT_MAX_VOTERS - 1` remote peers. Each peer
has explicit TurboRaft queue item, group and byte limits. Successful enqueue copies into the
SDK's bounded queue; rejection leaves the caller's payload unchanged. Receive views are borrowed
only during the callback. The runtime routes every Raft, snapshot and data frame by its actual
destination and never silently broadcasts or discards an unknown destination.

Runtime drive advances peer queues and the Raft clock; the host separately polls its clients.
The SDK's `SALTS_EBUSY` step result is ordinary idle/in-flight progress, while routing, admission
and transport errors are returned. Stop seals runtime admission before closing peer adapters.
The host keeps polling clients until terminal callbacks, then destroys the runtime before
destroying the borrowed clients. Destroy returns `SALTS_EBUSY` with ownership retained while a
peer is still live. A stopped runtime is terminal; restart constructs a new generation.

This replaces the removed peer-service abstraction rather than rebuilding it inside Flowie.
An alternative owned client pool would duplicate host progress, TLS and shutdown authority.
The selected approach changes only internal cluster composition; MQTT API, commit authority
and stored data remain unchanged. Cluster nodes must upgrade together; there is no legacy
transport fallback. Rollback selects the previous complete SDK/runtime set and recreates the
generation after closing its CNet clients; it never reconciles two simultaneous runtimes.

Creation order is owner projection, state machine, TurboRaft runtime, then DataStream router.
Startup begins peer service before accepting server/cluster work. Shutdown stops endpoint admission,
stops the TurboRaft generation, closes WalStorage, and finally destroys projections. A failed
stop or close is reported and does not silently switch to standalone mode.

## Verification

The server/cluster test set covers SQLite log recovery, snapshot restore, committed owner projection,
state-machine dispatch, CNet peer transport, DataStream quorum transfer and generation lifecycle.
Standalone repository tests cover only the MQTT V2 ORM schema.
