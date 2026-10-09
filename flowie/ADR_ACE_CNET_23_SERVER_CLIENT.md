# ACE + CNet 2.3: Flowie Server and Client ownership

**Status:** phased integration; this is not a claim of a released Salts 2.3 SDK,
a migrated MQTT Client or an implemented Component Configurator. Follow
[Flowie #42](https://github.com/qigao/flowie/issues/42),
[Server #56](https://github.com/qigao/flowie/issues/56),
[Client #61](https://github.com/qigao/flowie/issues/61),
[Configurator #62](https://github.com/qigao/flowie/issues/62),
[Benchmark #49](https://github.com/qigao/flowie/issues/49), and
[Salts #1081](https://github.com/qigao/salts/issues/1081).

| Responsibility | Owner | ACE / CNet primitive |
| --- | --- | --- |
| TCP/TLS listener, admission and fixed execution Owner | Salts::CNet | Acceptor–Connector, Owner Placement, bounded Handoff/Manager |
| OS readiness/completion and native SG progress | NativeIO/CNet | Reactor/Proactor, single backend observer |
| Remote outbound broker/peer selection | Salts::CNet | Destination Policy at dial/acquire, not per message |
| Physical client connection and reconnect | Salts::CNet | owner-local Manager/Managed Dial/Recovery |
| MQTT CONNECT/AUTH/CONNACK, QoS and session | Flowie | protocol FSM/ACK truth, generation fencing |
| MQTT fanout and retained/session index | Flowie endpoint | owner-local delivery, bounded cross-Owner descriptors |
| WS/WSS transport | CHttp | own CNet-backed Owner/transport lifetime |
| Cluster peer and consensus | FlowMQ/TurboRaft | independent transport peer and committed facts |
| Static service composition | Salts::Component + CMeta | Component Configurator, typed Strategy/Factory/Adapter |
| Dynamic provider lifetime | Salts::Plugin | outer ComponentPlugin generation Scope |

Do not create a Flowie Reactor, CNet-Actor bridge, global connection pool,
second CNet Manager DSO, plugin registry or per-packet Component lookup.
ACE Leader/Followers is **not** the SG fixed-owner model.

## Server transport slice (in this branch)

```text
listener -> canonical CNet RR/LOWEST_PRESSURE chooser
         -> advisory owner hints (eligible / handoff occupancy)
         -> authoritative cnet_handoff_reserve
         -> detached accept -> publish once -> wake final Owner
         -> take -> owner-local cnet_manager_reserve/adopt
         -> callback on fixed Owner -> real terminal / recycle
```

The existing Flowie `TF_NET_OWNER_ROUND_ROBIN` and
`TF_NET_OWNER_LEAST_CONNECTIONS` API remains a deliberately restricted,
source-stable facade. Both now delegate selection to the canonical CNet 2.3
policy implementation. Unknown values are rejected, not silently mapped.
`STRICT_KEY` is not exposed during TCP admission because MQTT ClientID is
unknown before CONNECT and established transports cannot migrate. Connection
and Manager/handoff credits are separate bounded resources.

CNet 2.3 exports Manager/Handoff within **Salts::CNet**; delete historical
linkage to the removed `Salts::CNetManager` target. The existing staging
connection tests cover RR/least occupied placement, owner affinity, stale
handle/generation, bounded credits, TLS/retained sends and shutdown; rerun
against the **exact** unified installed candidate before merge.

## Client contract (still to implement under #61)

```text
typed host-configured Client / ACE Component provider
 -> fixed local network Owner
 -> immutable allowed endpoint-set + CNet destination selection
 -> owner-local Manager, optional Managed Dial/Recovery
 -> transport CONNECTED
 -> Flowie MQTT CONNECT/AUTH/CONNACK and session validation
 -> cnet_managed_dial_protocol_ready(active generation ticket)
 -> subscription / QoS0/1/2 / settlement on same Owner
```

Local Owner affinity, remote broker endpoint selection and pool compatibility
are **three separate decisions**. STRICT_KEY selects by stable remote endpoint
identity, fails closed on an unavailable pinned winner and cannot override
TLS/authority policy. A single authenticated MQTT client owns one long-lived
session-bound connection; it does not acquire/release a generic physical pool
slot on every PUBLISH. A multi-client process may use an optional bounded
Owner-local connection set keyed by Owner, authority, trust/SNI/client identity,
protocol, broker and MQTT session/ClientID. No cross-Owner or cross-identity
physical reuse.

CNet reconnect only restores physical connectivity; MQTT CONNACK is the
protocol-ready boundary. PacketID, Receive Maximum, SUBSCRIBE restoration,
QoS1/2 settlement and PUBREL/PUBCOMP remain Flowie-owned. No automatic
retry/replay for an outcome-unknown PUBLISH or TLS security downgrade.
The existing Client has a dedicated, synchronous-polling worker and creates
its CNet client before that worker starts. Before Manager/Managed Dial or
mixed Server+Client SG cohosting, it must construct I/O on the **final Owner**,
adopt nonblocking incremental MQTT progress, and preserve a single backend
observer. In particular io_uring SINGLE_ISSUER is not transferable between
threads. This branch does **not** implement that migration.

## ACE Component Configurator / YAML contract (not yet implemented)

Flowie host YAML contains deployment facts, not an embedded `.flow` DAG.
A single validated typed representation feeds explicit
`cmeta_component_desc` manifests, `salts_component_deployment` and
`salts_component_selection`. The composition root calls
`salts_component_context_init/resolve/start/stop`, binds typed Interfaces
once, and drains in reverse dependency order. CNet builtin enum policies are
typed fields of network service configuration, **not** separately instantiated
Components. Static-only composition needs no Plugin DSO; dynamic providers
require an outer lease through every borrowed callable and completion.

YAML config is pre-start-only initially. No automatic hot reload, implicit
compatibility fallback or runtime Reflection lookup on the MQTT hot path.
The embeddable Flowie::Client and graph-neutral protocol module do not parse
YAML or create an implicit Configurator.

## Mandatory downstream gate

- An **immutable verified installed Salts 2.3 candidate**, rebuilt coherent
  producer SDKs, C11/C++17 and real Linux/Windows/macOS conformance (as supported).
- Server and Client-only, combined gateway fixed-Owner 1/2/4; real TCP/TLS,
  CHttp WS/WSS and FlowMQ cluster without double Observe/second Manager.
- CLIENT: TLS identity failure, stale reconnect tickets, MQTT CONNACK refusal,
  cancellation/late completion and QoS0/1/2 no-replay, bounded pressure.
- Component: real YAML preflight/DAG, failure-atomic activate/rollback, static
  providers, and dynamic Plugin Scope lifetime where used.
- Exact-HEAD CI, installed SDK consumers, real benchmark CPU/op, p99,
  copied/retained bytes, cross-owner hops and queue occupancy.

Do not mark the client migration or Configurator complete based on this
server-only transport slice. Stable release and master merge are separate gates.
