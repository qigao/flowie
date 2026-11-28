# Flowie MQTT application architecture

## Decision

Flowie MQTT flowie/protocol/session Core is graph-neutral. An embedded application may create and drive
that Core directly through `flowie_endpoint_core_*`; a TurboFlow adapter is an optional thin
composition layer over the same owner. The standalone `flowie_server` product assembles
Security, Repository and Endpoint through a static Salts Component Configurator, using
validated YAML or CLI configuration. It does not require a `.flow` Graph. Configured TurboFlow
applications can compose the same Core through their provider registry and Graph.
The flowie/protocol library alone is not an application. The normative terminology and the
composition forms are defined in
[`CONFIGURED_BROKER_CONCEPTS.md`](CONFIGURED_BROKER_CONCEPTS.md).

Flowie does not embed or link the TurboMQTT client, broker, socket, queue, worker, processor,
sink, or plugin runtime. The server application and
the independent `flowie_client` SDK share only the Flowie flowie/protocol module; the client SDK builds
its own single-owner flowie/transport state: Salts CNet for TCP/TLS and the independent Chttp client for
WS/WSS. The flowie/protocol knowledge migrated from TurboMQTT
remains deterministic and free of I/O:

- MQTT packet framing and fundamental constants;
- UTF-8, Topic Name, Topic Filter, and shared-subscription parsing;
- re2c/Lemon parser structure;
- legacy MQTT ACL text parsing, but not the ACL plugin or its lifecycle.

The initial source reference is TurboMQTT commit
`bb589e1812c66707fec32e2dccce0543121c54fc`. Every migrated public symbol is renamed to the
`flowie_mqtt_` namespace and is owned by this repository. Flowie behavior is verified against
the MQTT specification; the old implementation is a source reference, not a compatibility
authority.

## Alternatives

1. Link the existing TurboMQTT runtime. Rejected because its I/O, queues, processors, sinks,
   plugin lifecycle, and state ownership would bypass TurboFlow primitives.
2. Copy the complete TurboMQTT tree. Rejected because it would create two owners for sessions,
   subscriptions, inflight QoS, flowie/transport, and persistence.
3. Selectively migrate flowie/protocol-only code and rebuild the runtime from primitives. Chosen
   because parser state remains local while each mutable business state has one explicit owner.

## Layers and ownership

### TCP/TLS network owners

`network_workers` selects 1–64 fixed CNet progress owners for one endpoint; zero
keeps the existing single-owner behavior. The CLI accepts `--network-workers` and
YAML accepts `network_workers` in the endpoint adapter's `config`, alongside
`network_command_bytes` (CLI: `--network-command-bytes`). For example, four owners with the default 16 MiB
mailbox budget reserve 4 MiB per owner. The worker count cannot exceed the
connection limit, and every byte partition must hold one maximum-sized packet.
UDP/KCP rejects multiple owners with `SALTS_ENOTSUP`. WS/WSS delegates owner
count and placement to CHttp; its single Flowie wrapper retains an aggregate
mailbox, so the TCP/TLS per-owner byte-partition check does not apply.

`network_policy` (`--network-policy`) selects `round-robin` (the default) or
`least-connections`. The latter samples each owner's mutex-protected reservation
count, including pending handoffs and live connections, with rotating ties. Only
the listener increments reservations; concurrent closes may lower a sampled
count. This is a load hint, not a globally atomic minimum or a byte/CPU load metric.
Both policies skip full owners and preserve each connection's fixed owner.

`network_cpus` (`--network-cpus 2,4`, YAML `[2,4]`) optionally supplies exactly one
logical CPU per effective owner, in owner order. An empty YAML list or an omitted
option preserves OS scheduling; duplicate CPUs deliberately allow sharing. The
bounded list is copied into the endpoint and each connection configuration, with
no caller-owned storage retained. Windows IDs are `group * 64 + processor` and
Linux/Android IDs are OS logical CPU IDs below `CPU_SETSIZE`. Other platforms
reject explicit binding with `SALTS_ENOTSUP`. Explicit CPU binding is supported
only for TCP/TLS; WS/WSS accepts both placement policies but rejects CPU lists.
The listener thread is not pinned.

Each worker binds itself before polling or callbacks, then signals startup status
under its mutex. Start waits for readiness before creating the multi-owner listener
thread. A binding error fences and joins all started workers and returns the error;
the failed instance can be destroyed, but must be recreated before retrying.
The platform adapter uses [Windows thread group affinity](https://learn.microsoft.com/en-us/windows/win32/api/processtopologyapi/nf-processtopologyapi-setthreadgroupaffinity)
and [Linux thread affinity](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html).
This keeps platform calls below the endpoint and adds no dependency. Dynamic
migration and implicit automatic pinning were rejected because they complicate
connection ownership and can conflict with deployment CPU restrictions. Rollback
is to omit both options; the default remains round-robin with no binding. Consumers
must rebuild for the expanded configuration structs, as with `network_workers`.

With multiple owners, one dedicated listener thread reserves an owner slot using
the selected policy and performs detached accept. It moves
the accepted descriptor through that owner's bounded mutex-protected queue.
Pending handoffs and live connections share the same connection reservation;
the total never exceeds `max_connections`. If all owners are full, new arrivals
remain in the listener backlog. Admission failure releases the reservation and
closes the descriptor. After successful adoption, only the selected owner polls,
advances TLS, receives, sends, and closes that connection. It never migrates.

Handles keep the existing slot/generation layout. The global slot interleaves
owner-local slots, so simultaneous local slot 1 connections on different owners
remain distinct. Send, retained SG send, and close decode the owner before
entering its bounded MPSC mailbox; generations remain CNet's stale-handle fence.
Connection capacity and mailbox byte capacity are partitioned without increasing
their aggregate bounds. Command-entry capacity and the remaining CNet request,
event, and completion capacities apply per owner, so their total memory cost
grows with the selected count. Saturated mailboxes still return `SALTS_ENOBUFS`.
Scalar admission copies bytes; SG admission retains immutable slices. References
survive until CNet completion or cancellation, and queued references are released
at destruction after all workers have joined.

Callbacks for different network owners may overlap. The endpoint marshals them
to its existing serialized flowie/protocol executor, keeping session, subscription,
retained-message, and repository state under one authority. Receive bytes remain
borrowed until the synchronous executor call returns. This change parallelizes
network/TLS progress, not MQTT state execution; it does not establish a throughput
gain without measurement. Applications consuming `Flowie::Connection` directly
must make their observers safe for concurrent connections before opting in.

Shutdown fences command and connection admission on every owner first. The
listener closes on its thread; each network owner closes pending handoffs and
stops CNet through terminal callbacks. The caller joins all threads before
destroying queues, TLS contexts, or clients. A timeout leaves resources owned and
requires another stop call; destroy returns `SALTS_EBUSY` while workers remain.
An owner failure stops the group and is returned by stop. Lifecycle operations
remain exclusive and must run outside network callbacks.

Alternatives were one shared client polled concurrently (violates CNet ownership),
per-owner reuse-port listeners (platform-dependent distribution), and flowie/protocol
state sharding (requires a separate repository/session ownership design). Detached
handoff preserves one portable listening socket and the current MQTT authority.
The appended endpoint config fields require SDK consumers to rebuild; existing
source initializers keep single-owner defaults. Roll back configuration to zero
or one to restore the existing direct-accept path. Regression coverage belongs to
the connection and MQTT flowie/transport suites, including owner affinity, retained SG,
global capacity, stale handles, and shutdown with active connections.

Flowie endpoint Core owns its `Flowie::Connection` listener and accepted connection handles; it does not depend on
or compose a generic `io/socket` adapter. The optional TurboFlow endpoint adapter injects a graph
dispatch sink into that Core and exposes graph operations without duplicating state. Reusable code below this boundary is limited to the
flowie/protocol-neutral Salts executor wrapper, Salts CNet, and the independent Chttp server runtime under
the dedicated `flowie/transport` and `connection` modules. Chttp is linked by role: `CHttp::Client` owns outbound HTTP/WebSocket operations,
while `CHttp::Server` owns HTTP/WebSocket listeners. Salts remains the source of CNet, Core,
Coroutine, and shared error codes.

### Control dashboard rendering

The dashboard uses `CHttp::App` with its Jinja renderer and read-only CMeta
models. HTTP routes, session authority, authorization, database schema and browser
forms remain owned by Flowie Control. The shipped SDK versions are selected through
[the dependency configuration](../README.md#upstream-dependency-versions).

Each dashboard view owns one renderer and serializes render calls with a mutex.
Request models and borrowed strings remain alive until synchronous rendering
returns. The renderer copies a fixed template bundle at startup, bounds template
source to 512 KiB and HTML to the existing 16 MiB limit, and enables HTML
autoescaping. Complete owned HTML passes to the existing copying response path.
Seven fixed section templates keep individual templates within the SDK limits;
request input cannot select arbitrary filesystem templates. Destruction follows
HTTP worker shutdown so no model or renderer remains borrowed.

Form decoding uses App's bounded parser while retaining Flowie's stricter
command admission: explicit separators, unique decoded keys and no embedded
NUL. Decoded password storage is wiped on both success and failure. Keeping the
old renderer alongside App would duplicate presentation and escaping policy;
rollback instead selects the previous complete image and its template bundle.
No persisted-data migration is needed. Formal dashboard regressions cover
concurrent models, attribute escaping, command forms, selectors and pagination;
HTTPS integration exercises the same route and session boundaries.

In standalone mode MQTT flowie/protocol facts have one source of truth: Flowie's typed flowie/protocol repository
over `Orm::C`. Session, subscription, inflight, retained, and Will mutations commit to that
repository before Flowie swaps its owner/cache state. The in-process vectors, maps, and topic
trie are rebuildable indexes and scheduling caches only; they must never advance independently or
serve as a fallback fact source. The standalone executable opens the configured TurboDB
driver and defaults to a persistent SQLite file. The connection is owned by the repository,
which restores the broker indexes from durable records at startup. Application hosts can
explicitly select an in-memory SQLite repository for ephemeral state. Graph-owned business
sinks have their own storage and cannot restore protocol state.

Cluster mode has a different, exclusive owner. Business owner/PUBLISH commands enter
`TurboRaft::Service`; the Flowie state machine applies them only after quorum commit.
`TurboRaft::WalStorage` owns term/vote, the Raft log, and snapshots. The in-memory owner directory
is rebuilt from committed state and has no independent write path. Cluster composition therefore
does not create an ORM, Redis, PostgreSQL, FlowStore, route store, or outbox fact source.

Endpoint registration installs the `protocol.mqtt.server` module catalog. The graph-visible
operations are `mqtt.publish.ingress` for an admitted application PUBLISH and
`mqtt.packet.egress` for an encoded routed packet. They bind to the existing endpoint owner;
CONNECT, SUBSCRIBE, UNSUBSCRIBE, PING, AUTH, QoS transitions, retained state, and session
persistence remain internal owner behavior rather than invented graph operations.

| Layer | Owner | Mutable state | Forbidden dependencies |
|---|---|---|---|
| `flowie_protocol` | caller-owned parse invocation | none; results borrow input bytes | Salts CNet/Chttp, sockets, queues, graph runtime, storage |
| `flowie_client` | DLL-owned Salts worker | one outbound Salts CNet or Chttp WebSocket flowie/transport, framing, packet IDs, inbound QoS 2 state, bounded async command queue | Flowie endpoint, graph runtime, server session or persistence state |
| endpoint Core | Flowie endpoint owner | listener lifecycle, limits, connection registry | direct callback or graph adapter mutation of connection/session state |
| optional graph adapter | TurboFlow composition | source name, route-owner registration, graph settlement result | socket/session ownership |
| connection resource | Flowie network owner | generation-fenced flowie/transport handle, receive buffer, negotiated version | MQTT parser performing reads or writes |
| session owner (internal) | Flowie session owner | bounded cache reconstructed from session facts | independent public resource identity or queue/sink state advancement |
| subscription index | Flowie subscription owner | rebuildable filter/member query index | graph-owned subscriber membership |
| application graph | TurboFlow | private message attempt and settlement result | direct MQTT ACK, reconnect, or socket access |
| standalone flowie/protocol repository | Flowie typed repository over `Orm::C` | session/subscription/inflight/delivery/retained/Will facts | server/cluster state, Graph sink writes, backend fallback, or a second fact source |
| server/cluster Raft state machine | `TurboRaft::Service` with WalStorage snapshots/log | committed owner and publish data; in-memory owner directory is derived | ORM/Redis/PostgreSQL storage, independent route writes, or apply before commit |

Parser output is a zero-copy borrowed view. The connection owner must either finish all use
before the receive buffer changes or copy selected fields into its own bounded session/message
storage.

The client SDK owns one persistent worker thread. Managed operations deep-copy their packet descriptions
into a queue bounded by both command count and owned bytes. Only the worker mutates the MQTT
state machine or flowie/transport; cross-thread submissions wake that owner through the bounded command queue. Completion,
inbound PUBLISH, and unsolicited-disconnect callbacks run on the worker and may enqueue more async
commands, but may not perform synchronous client I/O or destroy the client. Destruction marks the
queue closed, interrupts pending I/O, completes accepted queued commands with `SALTS_ESHUTDOWN`,
joins the worker, and only then frees the Salts CNet/Chttp flowie/transport and client state.

Stream flowie/transport and application dispatch are separate bounded paths:

- connection lane: socket recv -> framing/parser -> complete MQTT packet;
- direct admission: owned packet -> required Core callback; or
- graph admission: owned packet -> optional TurboFlow adapter/source -> inline/worker stage.
- reply admission: encoded flowie/control packet + message-owned route -> bounded endpoint Queue ->
  owner-lane route lookup -> Salts CNet or Chttp WebSocket send.

When `manage_sessions` is enabled, CONNECT is the flowie/protocol-owner exception to application
publication. The endpoint's private session primitive consumes the complete CONNECT on the same
endpoint owner lane, authenticates and authorizes it when an explicit security binding is installed,
opens or resumes the bounded session, atomically rebinds
the provisional connection route to the session generation, and enqueues CONNACK. Rejected and
accepted CONNECT packets do not enter application dispatch. SUBSCRIBE, UNSUBSCRIBE, PUBREL,
PINGREQ, DISCONNECT, and enhanced AUTH are also consumed by that flowie/protocol owner; only admitted
PUBLISH packets enter the selected direct callback or optional graph adapter.
When `manage_sessions` is disabled, the endpoint preserves external-session mode and publishes
CONNECT for a separately assembled session processor.

Each MQTT connection has exactly one CNet-lane owner for socket receive, framing, parser,
negotiated flowie/protocol state, close, and backpressure. Raw stream bytes never cross a lane boundary.
The first client packet must be a valid CONNECT. Its flowie/protocol level fixes MQTT 3.1 level 3,
MQTT 3.1.1 level 4, or MQTT 5 level 5 for all later packets on that connection; a non-CONNECT
first packet or a second CONNECT is a terminal flowie/protocol error. The CONNECT flowie/protocol-name/level
pair is exact: `MQIsdp`/3 or `MQTT`/4|5.
Connections using all three supported levels may coexist in one endpoint. Fan-out is encoded for
the subscriber session's negotiated level: MQTT 5 properties are forwarded only between MQTT 5
sessions, are removed for MQTT 3.x subscribers, and an MQTT 3.x publication becomes a valid MQTT 5
PUBLISH with an empty property block for an MQTT 5 subscriber.

Level 3 is a compatibility dialect, not a relaxation of the parser's security boundary. It keeps
strict UTF-8 validation and the legacy 1--23 byte Client Identifier limit. Because level 3 has
neither CONNACK Session Present nor a negative SUBACK code, Flowie keeps resumed-session state
internally but emits a zero CONNACK flag, and closes on subscription ACL/capacity rejection instead
of reporting false success. MQTT-over-WS was standardized after MQTT 3.1, so accepting level 3 over
WS/WSS is an explicit Flowie flowie/transport extension. Protocol differences are tracked against the
[MQTT.org 3.1/3.1.1 comparison](https://github.com/mqtt/mqtt.org/wiki/Differences-between-3.1.0-and-3.1.1)
and the [OASIS MQTT 3.1.1 specification](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html).
The tested Flowie WS/WSS path uses the WebSocket subprotocol token `mqtt` for MQTT levels 3, 4,
and 5, with the CONNECT flowie/protocol-name/level pair remaining the version fact source. Some legacy
MQTT 3.1 clients instead require `Sec-WebSocket-Protocol: mqttv3.1`; Flowie does not currently
claim or test that token. This token difference does not affect MQTT 3.1 clients using TCP, TLS,
over TCP or TLS.
The public callback client accepts an optional verified TLS identity for TLS and WSS plus
MQTT 5 Enhanced AUTH challenge and re-authentication callbacks. CA, certificate, key, and password
strings are copied at creation; certificate and key are an atomic pair, peer verification cannot
be disabled, and the copied key password is wiped at destruction. Only the current complete client
configuration layout is accepted.
The framing buffer owns incomplete bytes only; it is neither a durable Queue nor an ACK fact
source. Once parsing identifies a complete PUBLISH packet, Flowie materializes the complete wire
packet in a reference-counted `mem_buffer_t` carried by `turbo_flow_msg_t`, consumes the framing
bytes, and performs one dispatch to the injected sink. Managed CONNECT is instead
consumed by the same-lane session primitive described above. Only a complete owned message may
cross an execution boundary. A direct Core callback runs synchronously on the owner dispatch;
the optional graph plan may select inline execution or a bounded worker Disruptor. A dispatch failure is returned to the connection/session owner
and is never retried implicitly by the framer. Protocol, HWM, and graph errors put that
connection ingress into a terminal state; its owner must close or rebuild the connection before
accepting more bytes.

## MQTT broker processing stages

Flowie separates the MQTT flowie/protocol owner from the application message graph. The owner is the
only authority for connection, authentication, session and QoS state; the graph is a programmable
pipeline for messages that have already crossed that security boundary.

```text
socket/frame
  -> parse, size and flowie/protocol validation
  -> CONNECT authentication and operation ACL
  -> session/inflight admission (owner lane)
  -> turbo_flow_msg_t (owned buffer + MQTT metadata)
  -> optional TurboFlow Policy filter/route/transform
  -> optional store/durable boundary
  -> optional after-process stages
  -> MQTT fan-out / HTTP / socket / Redis sinks
  -> settlement result returned to the session owner
  -> flowie/protocol ACK when the configured prerequisite is complete
```

The first three steps are mandatory owner work; authentication and ACL are mandatory whenever a
security binding is configured, and the graph can never bypass that binding. A message that fails
authentication, ACL, flowie/protocol validation, inflight limits, or packet-size limits never reaches a
user graph. A graph stage may reject or transform an admitted PUBLISH, but it may not emit CONNACK,
PUBACK, PUBREC, SUBACK, or otherwise mutate a session generation. TurboFlow Policy facts are evaluated over the
versioned MQTT metadata and payload view already attached to `turbo_flow_msg_t`; it does not read
the connection receive buffer or a live socket.

TurboFlow can process any provider message represented by `turbo_flow_msg_t`. The current Flowie
endpoint contract exposes only `mqtt.publish.ingress` and `mqtt.packet.egress` to the graph;
CONNECT, AUTH, SUBSCRIBE and other flowie/control packets remain owner-internal. Exposing a future
normalized flowie/control-event source would require a separate flowie/protocol contract for authorization,
session mutation and ACK ownership; it is not implied by adding a generic Policy stage.

The remaining stages are composition points, not mandatory broker behavior:

- **accepted** is completed explicitly by the selected graph admission stage; it is not tied to a
  hidden queue;
- **store** commits a selected durable record (for example Redis Stream or a ORM repository
  adapter) and is the `durable` settlement point;
- **after-process** is an optional post-routing side effect or audit stage. Its failure is
  part of the synchronous graph result when it is connected to the selected path.

The graph may place `store` before Policy when the durable fact must be the original admitted
packet, or after Policy when the durable fact must be the transformed/filtered message. These
are different topologies and must be selected explicitly in `.flow`; Flowie does not infer or
silently reorder them. A branch that is not part of the configured settlement prerequisite is
not created implicitly. To keep after-process work out of ACK latency, the graph must first commit
an explicit accepted/durable handoff and run that work from its separately consumed path;
the original synchronous graph cannot silently ignore a selected branch failure.

## Pattern composition

Flowie and FMQ share flowie/protocol-neutral pattern vocabulary and algorithms, not the FMQ v3 wire
codec. Flowie currently uses the core candidate iterator for ordinary/shared subscription
selection and its generation-fenced route matcher for live connection lookup:

- ordinary MQTT subscription: PUB/SUB fan-out over the subscription index;
- shared subscription: PUSH/PULL selection within one share group;
- MQTT 5 Response Topic and Correlation Data: an MQTT-owned correlation use case, not currently
  bound to the core synchronous exchange state;
- connection and session addressing: ROUTER/DEALER-style logical routes;
- delayed graph completion: settlement owned by Flowie, after which Flowie emits the MQTT ACK.

The reusable core owns selection, route matching and synchronous correlation state, but it does
not own an MQTT socket or emit an MQTT packet. FMQ and Flowie adapters translate their own
wire-level identity into the applicable flowie/protocol-neutral contracts; unused core mechanisms do
not imply a Flowie feature.

## Two ACK classes

Transport/flowie/protocol ACK and application settlement are separate facts:

1. **Protocol ACK** is emitted only by the Flowie session owner. It advances MQTT QoS state and
   is governed by the configured received/accepted/processed/durable settlement point.
2. **Storage/application ACK** is returned by a selected local, Redis, PostgreSQL, or sink
   primitive after its own write contract succeeds. It never writes directly to the client.

For a durable policy, successful graph processing without successful durable commit is not an
ACK. For an accepted policy, ownership transfer into the configured bounded primitive is the
commit point. No implicit fallback between policies is allowed.

## Configuration contract

The standalone `flowie_server` accepts listener options or a YAML file selected with
`--config` and `--profile`. The profile selects an endpoint and, when configured, HTTPS
Auth/ACL providers. Standalone protocol storage is selected by CLI or environment. The
[server guide](SERVER_GUIDE.md) describes validation and the exact startup commands.

The optional TurboFlow host described in the [composition concepts](CONFIGURED_BROKER_CONCEPTS.md)
owns Graph DSL, RuleSet and business adapters. Its Graph explicitly binds sources, sinks and
settlement points. A YAML endpoint and a TurboFlow Graph have separate ownership and validation.
The settlement-, retained-, slow-subscriber-, coroutine-capacity-, and receive-buffer-aware endpoint
configuration is the only public endpoint layout. Local endpoint/client ABI version numbers are not
part of the API; MQTT 3.1/3.1.1/5 remain the flowie/protocol version facts.
TurboFlow graph topology remains DSL; `flowie.yml` does not invent an unsupported `flows` root.

### MQTT 5 managed-session contract

The managed endpoint negotiates Receive Maximum and Maximum Packet Size in both directions. The
connection owner charges the client Receive Maximum for QoS 1/2 PUBLISH on the current connection.
An acknowledgement releases a slot only if that connection sent the corresponding PUBLISH.
If the window stalls the front of the send queue, control packets, including replayed PUBREL,
can advance using the same queue and byte budget. Pending PUBLISH packets keep their relative order.
Online and offline deliveries remain queued until their first send attempt. A persistent endpoint
commits that transition before handing the packet to the network owner, so a restart during the
handoff conservatively sets DUP on replay. The broker skips PUBLISH packets
that exceed a recipient's Maximum Packet Size, including retained and offline replay.

The TCP/TLS owner admits a terminal MQTT response after previously admitted writes. CNet retains
the bytes until the write settles. TLS uses the CNet final-write close. TCP requests send-side FIN
and discards late input. The 500 ms wait for peer EOF starts when CNet completes the last write.
The transport write timeout applies while the write is pending. The endpoint stops MQTT admission
when it submits the final write. The bounded command mailbox owns the bytes until CNet accepts them.

During session replacement, the endpoint stages an owned copy of any due predecessor Will before
committing the replacement. Persistent endpoints save that Will and the replacement session in one
transaction. The commit must succeed before the old connection is fenced or the Will is eligible for
dispatch. Startup restores one pending predecessor Will per session. The expiry task keeps it in the
repository until application dispatch accepts it. A process crash after acceptance and before record
deletion can repeat dispatch. [Repository upgrade and rollback](ADR_TYPED_ORM_PERSISTENCE_V2.md).

The standalone managed endpoint serves a read-only, retained QoS 0 `$SYS/broker/version` message to
authorized matching subscriptions. It is generated from the build version on subscription, follows
Retain Handling, and does not occupy a repository slot. Other `$SYS/` topics follow the normal ACL
and retention rules. Root wildcard subscriptions do not match topic names that begin with `$`.

After CONNACK, the connection owner measures MQTT idle time from the last complete incoming packet.
The deadline is 1.5 times the CONNECT Keep Alive, capped by an explicitly tighter endpoint timeout.
A zero CONNECT Keep Alive disables this MQTT deadline. The endpoint expiry task closes idle
connections through the normal connection-loss path. Anonymous listeners return CONNACK reason
0x8C when a client requests an unsupported Authentication Method. An empty
clean-start Client Identifier is replaced by a UUIDv7-based identifier returned in CONNACK; an empty
non-clean identifier is rejected. `topic_alias_maximum` bounds a connection-owner alias table. Alias
resolution occurs before ACL evaluation, session admission, and graph publication, so downstream
code never authorizes an empty or stale topic.

A resume of the same MQTT 5 session within its Will Delay suppresses the old Will. A fresh MQTT 5
session ends the old session, and an MQTT 3.x takeover closes the old connection with its Will.

Subscription Identifier is owned by each subscription and deduplicated across overlapping matches
for one delivery. It is appended to the outbound MQTT 5 PUBLISH while inbound Subscription
Identifier properties are not forwarded. Canonical session records now write FSES 1.3. FSES 1.2
added the bounded identifier to the subscription options record; FSES 1.3 extends each outbound
delivery metadata record with its absolute Message Expiry epoch deadline. Restore accepts FSES
1.0-1.3: FSES 1.0/1.1 restore Subscription Identifier as zero, and FSES 1.0-1.2 restore without a
broker delivery-expiry deadline because those layouts did not preserve that fact.

The session owner prunes expired PUBLISH deliveries before reconnect replay and the connection
owner checks the same deadline again before a queued socket send. A non-expired MQTT 5 replay
derives its remaining Message Expiry Interval from the absolute deadline and rewrites only the
fixed-width four-byte property value. The absolute deadline remains the fact source; the wire value
is a derived view. MQTT 3 outbound deliveries retain the same broker deadline but have no expiry
property to rewrite. Once QoS 2 advances to `WAIT_PUBCOMP`, the stored packet is PUBREL rather than
PUBLISH and its delivery expiry is cleared. New writers never downgrade records. An older FSES 1.2
reader cannot consume FSES 1.3 records, so rollback requires draining/removing 1.3 session records or
using a rollback binary that also understands FSES 1.3.

### Settlement compatibility, migration, and rollback

`FLOWIE_ENDPOINT_CONFIG_INIT` and an omitted YAML settlement field retain the legacy `received`
behavior. The receive boundary commits the session transition and admits its ACK to the
connection-owned bounded reply Queue before graph publication. Socket scheduling may overlap graph
execution, but graph success is not an ACK prerequisite: if publication terminates with an error,
the endpoint drains that already committed ACK and then closes the connection. This exception is
limited to flowie/protocol replies whose owner transition already committed; it does not turn graph
failure into success or keep the connection accepting new work.

For locally managed persistent retained PUBLISH packets using `received` or `processed`
settlement, the endpoint queues the QoS 1/2 settlement behind any fanout admitted by the
application callback. The fanout applies the retained write or deletion before the protocol owner
can send PUBACK or PUBREC. A failed fanout closes the publisher and discards the pending ACK.
This ordering adds storage latency to retained acknowledgements. It uses the existing repository and
reply queue and requires no storage migration. Roll back by restoring the previous server version.

| YAML value | ACK prerequisite | Required graph boundary |
| --- | --- | --- |
| omitted or `received` | Session owner receive transition | None; legacy-compatible default |
| `accepted` | Explicit graph ownership-transfer commit | Selected admission stage completes the settlement envelope |
| `processed` | Successful synchronous graph publication | All configured inline/worker stages complete |
| `durable` | Successful persistent commit | PostgreSQL COMMIT or Redis Stream XADD acknowledgement |

QoS 0 remains receive-settled and has no MQTT ACK. Moving away from `received` changes ACK latency and the
failure/redelivery window, so deployment must quiesce ingress, drain or explicitly disposition
inflight work, stop the endpoint, change the YAML policy and graph boundary together, validate the
resolved profile, and then restart it. These fields are not hot-reloaded. Rollback uses the same
sequence and explicitly restores `received` (or removes both settlement fields); rolling back only
the graph while leaving `accepted` or `durable` configured fails instead of silently acknowledging
at another boundary. Endpoint config ABI v8 is the public layout for this contract.

## Worker process topology

`flowie_server` owns the MQTT listener, endpoint lifecycle and protocol repository.
`flowie-control` owns its HTTPS listener, Repository and management services.
The bundled Compose entrypoint starts both child processes, forwards termination signals,
and terminates the remaining child if the other exits. Native deployments can supervise
the executables independently. [Deployment guide](../deploy/server/README.md).

The legacy `flowie_supervisor` source records a separate Worker process design. Current CMake
builds the Broker executable and Control tools. External TurboFlow hosts own Graph startup and
shutdown when composing the broker core.

Security uses the same composition rule. YAML selects `security_realm`, its exact HTTPS
`policy_source`, and `auth_method`; it does not contain ACL rules or a policy version. The host
creates the HTTPS ACL provider, binds its borrowed interface to the realm, registers the realm as a
resource, and injects the typed realm plus HTTPS authentication provider through
`flowie_endpoint_security_binding_t`. The endpoint copies only the validated principal into session
state and never stores password/authentication bytes. Version mismatch or expiry triggers a bounded
provider refresh; normal authorization executes only against a refcounted immutable local snapshot.
The binding's channels and method must equal YAML exactly, so stale or miswired composition fails
before registration. There is no realm pointer lookup, service locator, plaintext user database,
YAML policy body, provider fallback, or implicit anonymous fallback. The complete flowie/control/data-plane
decision is documented in `ADR_DYNAMIC_ACL_BUNDLE.md`.

The endpoint also owns the provenance of flowie/transport authentication context. TCP/TLS/UDP/KCP/WS/WSS
`remote_address` is the numeric direct network peer. A TLS/WSS
endpoint may explicitly require trusted PROXY v1/v2 admission before TLS; only a direct peer in the
configured numeric CIDRs can then supply the source address, while the direct flowie/transport peer remains
available separately. Flowie does not consume HTTP forwarded headers. When an endpoint configures
`tls_client_ca_file`, CNet requires and verifies the MQTT client certificate before Flowie reads
its canonical SHA-256 fingerprint. These values cross the provider ABI as borrowed, request-lifetime
fields and the certificate append is guarded by the request `size`. Broker HTTPS Auth v3 forwards
them to `flowie-control`; the latter still derives Domain from the separate verified mTLS
identity of the Broker-to-flowie/control connection. Neither address nor MQTT client certificate is an ACL
fact source: the flowie/control Repository remains the only ACL owner and Broker authorization remains a
local immutable snapshot lookup.

## Failure and rollback

- malformed flowie/protocol input fails at the connection boundary and does not enter the graph;
- authorization denial does not allocate session/application work;
- a graph or persistence failure leaves the session inflight record at its previous committed
  generation and follows the configured retry/disconnect policy;
- source migration rollback requires deploying the previous complete TurboFlow release; Flowie is
  part of the invariant product build and cannot be removed with a CMake feature switch;
- no TurboMQTT runtime file is modified, so source migration can be audited or repeated without
  changing the original application.

## Verification gates

1. flowie/protocol framing, fragmentation, malformed input, limits, UTF-8, topic, shared-filter, and ACL
   parser tests;
2. endpoint/session tests with synthetic primitive completions and no network;
3. real CNet TCP/TLS/UDP/KCP runtime tests plus CHTTP WebSocket flowie/protocol integration tests;
4. standalone ORM transaction/recovery tests and server/cluster TurboRaft log/snapshot replay tests;
5. Product acceptance requirements and the current SDK pipeline are documented in
   [SDK qualification and release requirements](RELEASE_GATE.md).

## Domain Release HTTP redesign

The standalone IoT gateway separates **Domain Configuration** and **Domain Packages**.
The deployment YAML configures listeners, TLS trust, the MQTT identity source, and the
downstream connections. The management HTTPS listener verifies and activates named
`.tar.zst` packages. Their signed manifests bind the Domain, environment, base digest and
file inventory. The active package controls ACL permissions, MQTT source bindings, RFL
rules, DataBind schemas and ToadScript modules. The package refers to downstream connections
by the sink names declared in the deployment YAML.

The management listener can use Control to authorize human sessions and per-Domain release
Service Accounts over management mTLS. Control owns account and credential lifecycle, while
the gateway owns package storage, validation and activation. [Account setup and permissions](CONTROL_GUIDE.md#domain-packages-账号与远程授权)

The gateway uses the [HTTPS Auth contract](ADR_HTTPS_AUTH_SERVICE.md) for MQTT CONNECT when
that identity source is configured. The Auth service resolves the canonical principal and
the gateway authorizes that principal with the active package. Flowie owns MQTT ingress,
worker lifecycle and delivery. RulesForge owns the window and rule state for each processing
generation.

[The Domain Release ADR](ADR_DOMAIN_RELEASE.md) defines the archive and HTTP contracts,
certificate policy, bounded Zstd decompression and activation fencing.

### HTTP contracts and ownership

| Surface | Responsibility |
| --- | --- |
| `POST /domains/{domain}/releases` | Verify the named, signed `application/zstd` package and its USTAR contents. Store the immutable candidate by SHA-256 of the compressed package. |
| `POST /domains/{domain}/releases/{digest}/validate` | Compile the signed ACL, check the rules and configured sink bindings, and report whether the candidate can activate. The activated package serves traffic. |
| `PUT /domains/{domain}/active-release` | Select a validated digest. Check its signed base and HTTP precondition in the activation transaction. Require `If-Match` for replacement and `If-None-Match: *` for initialization. Record the acting principal, signer, digest, and new generation. |
| `GET /domains/{domain}/releases`, `GET /domains/{domain}/releases/{digest}`, and `GET /domains/{domain}/active-release` | List release history, read a verified package by digest, and read the committed generation and its strong ETag. |
| `GET /healthz` and `GET /domains/{domain}/status` | Report management listener liveness and Domain release state. The current status endpoint reports committed and observed generations and MQTT ACL readiness. Queue depth, connectivity, and delivery fields return `null`. |

The release API replaces the JSON-RPC and Dashboard configuration write surface. CLI and CI
manage releases over HTTPS. Runtime owners authenticate principals, authorize operations,
and move business data. The Broker binds the configured authentication
provider and evaluates the activated ACL snapshot. Identity setup, credential issuance and
revocation belong to the identity provider or its operator tools. MQTT connection, session,
retained, and inflight state belongs to the Broker and protocol repository. Business records
belong to the configured storage owners. Data deletion and migration are separate operations
owned by those stores.

Gateway management HTTPS publishes the release owner's observations. The Portal backend owns
the Monitor query and subscription interface and obtains business outcomes from the services
that execute those operations. [Status ownership and subscriptions](ADR_DOMAIN_RELEASE.md#status-ownership-and-subscriptions)

### Release admission

The Control repository is the authority for the active Domain generation. CMeta Component
Configurator resolves the runtime's typed providers and rejects missing or ambiguous bindings.
The upload path authenticates the caller before accepting package data and enforces limits on archive
bytes, expanded bytes, and entries. Validation checks the signer scope, file digests, and the
exact set of files. The platform provides the management policy and signing trust from outside
the archive.

### Release certificates and algorithm policy

Control uses `Salts::Crypto` for release signatures and content digests. The readable USTAR
payload contains manifest bytes authenticated by detached SM2/SM3 CMS SignedData. The configured
profile selects the CMS OID family and digest algorithm explicitly. The signed manifest binds
each payload entry's path, length and SM3 digest. Verify the exact manifest bytes before
interpreting them, then compare the full set of entries and stream their digests under the
archive limits before preparing any release component. Keep the manifest within the SDK's
64 KiB CMS content limit and allocate bounded verification buffers outside the I/O coroutine
stack.

Deployment configuration authorizes signing identities for a Domain and target environment.
For SM2 release signing, provision the trust anchor, expected leaf key, explicit leaf-first
certificate path and SM2 signer/issuer IDs outside the archive. Require `digitalSignature` and
the `codeSigning` EKU and validate at the server's admission time. Select revocation explicitly.
Where required, use the `Salts::Crypto` SM2 CRL verifier with operator-managed, authenticated
CRL snapshots and freshness and rollback limits. Reject unapproved profiles, signature or
digest mismatches, and any failure required by the chosen trust policy. Recheck that the
signer remains authorized when activating a stored release. New signature algorithms require
an explicitly enabled release profile with its own verifier and trust policy.

SM4-GCM is available through the SM2 CMS envelope API for opt-in recipient encryption when a
separate artifact requires confidentiality. Release admission uses the signed `.tar.zst`.
HTTPS and MQTT TLS keep their own server or client certificate profiles and trust roots. A
release-signing certificate authorizes artifact publication only.

### Generation preparation and activation

Prepare the complete candidate before changing the active record. Preparation acquires validated
resource bindings and builds graphs. Message admission and delivery start at activation.
When activation commits, workers may admit new work only at the active generation. A worker
that has yet to load that generation reports itself as unready for the Domain. Status records
the committed generation independently from each worker's observed generation. Old scopes can
drain admitted work under their original configuration. The sink owner serializes the current
permission check with external-write admission. This fences writes from old scopes to revoked
destinations. The Broker owns MQTT acknowledgement and settlement.

Track the ACL policy version separately from the Domain release generation. A release with
the same ACL preserves the ACL version. After an ACL change, the Broker evaluates
protected operations under the new snapshot. It refreshes the principal through a supported
authentication flow or requires a new connection when the principal's policy version is stale.
Repeated activation of the same digest with the same idempotency identity returns the original
result. A competing activation fails its precondition and leaves the committed release intact.

### Domain forwarding and downstream delivery

The Flowie frontend receives MQTT messages, checks identity and ACL, and admits
events to the committed Domain generation. The signed Domain rules execute in a
resident RulesForge Program-backed Continuous Session. The session runs
event-time windows and ToadScript, and emits actions with a backend binding name
and payload. The RulesForge SDK worker runs in a separate process.

Flowie validates each output against the named deployment binding for its generation
and sends an mTLS HTTPS POST to the downstream processor. The frontend commits the event
to a SQLite inbox before QoS 1/2 acknowledgement. The backend records rule execution,
the output intent before sending, and the delivery result before retiring the task.
An accepted POST records delivery to the configured receiving endpoint. The downstream
service owns completion of its business operation.

The current ACL release profile can manually submit Praktor instructions to a separate
receiver. The internal FIFO and HTTPS sink exercise event-correlated decisions in loopback.
The RulesForge worker and its output batch integration are specified in the
[Domain Release ADR](ADR_DOMAIN_RELEASE.md#ingress-rulesforge-pipelines-and-downstream-business-services).

### Cutover, rollback, and verification

Choose a clean service cutover. Extending the existing RPC mutation methods would give the
signed package and mutable drafts independent paths to active configuration. A compatibility
facade would also retain the old direct-publish and bootstrap contracts. Keeping the HTTPS
transport and replacing the application contracts lets the release repository enforce one
activation authority.

Before cutover, stop legacy configuration writes, take a consistent repository backup, and
export and sign each Domain's initial release. In a separate deployment, verify these packages,
prepare their components, and exercise Broker Auth/ACL and representative input-to-output flows.
Replace Control, the Broker's Control HTTP security adapter, the provisioning tool, and
CI API bindings as one deployment step. Keep the original runtime and backup available
for deployment recovery.
After accepting new state in the redesigned service, recovery to the old runtime requires
reconciling or restoring that state explicitly. A configuration rollback within the redesigned
service re-signs prior content against the current base, verifies it against current permissions,
and activates it as a new generation.

Acceptance covers archive limits, signature and digest failures, cross-Domain access, duplicate
upload and activation, concurrent activation, candidate startup failure, generation drain,
stale status observations, and cutover with existing MQTT connections. The initial end-to-end
gate uses a signed ACL release and real local MQTT PUBLISH and SUBSCRIBE outcomes. The full
forwarding gate exercises a Flowie route through a real egress sink, downstream timeout,
a saturated queue, and a generation change during forwarding.

## Current implementation boundary

The packet envelope, CONNECT/PUBLISH/SUBSCRIBE/UNSUBSCRIBE typed decoders, bounded
CONNACK/PUBACK/PUBREC/PUBREL/PUBCOMP/SUBACK/UNSUBACK/PINGRESP/DISCONNECT/AUTH codec,
MQTT 5 property iterator,
UTF-8/topic/shared-filter/ACL parsers, PUBLISH flowie/protocol/data bridge, and SecurityRealm MQTT
matcher are implemented with focused tests. The bidirectional `flowie_endpoint` adapter now owns
TCP/TLS/UDP/KCP listener startup through CNet and WS/WSS startup through CHTTP WebSocket, accepted connections, same-owner receive/framing/parser
state, connection limits, stop/drain, and Connection, QueueBuffer, plus ProtocolAggregate
resource snapshots. Incomplete receive bytes remain private connection state and are not a Queue.
The QueueBuffer resource accounts for the real bounded outbound path. Cross-thread commands first
enter one endpoint owner-lane queue. After route and flowie/protocol validation they move, without losing
their aggregate reservation, into the target connection's TurboUtils deque. `send_hwm_bytes`
bounds each connection's pending wire bytes; the endpoint aggregate byte capacity is the checked
product of that limit and `max_connections`.

TCP/TLS small-packet storage is lowered privately by `Flowie::Connection` after
canonical slice and total-send-size validation. Each already-ready command keeps
its FIFO position and original byte reservation. Adjacent ranges of at most 256
bytes are copied in runs of at least two into one Salts pooled buffer, with at
most 4096 copied payload bytes per command. Large ranges and isolated small
ranges keep their original retained backing. Mixed runs share the same packed
buffer while remaining on either side of their intervening large ranges. The
metadata is bounded by `CNET_RETAINED_VECTOR_MAX`; planning is O(n), copying is
O(copied bytes). These conservative internal bounds are not a measured optimum.

This chooses bounded copying over always-retained SG for tiny fragments because
CNet feeds TLS plaintext ranges separately: one logical SG write does not promise
one TLS record. The tradeoff is an extra bounded allocation/copy, temporary
overlap with the source storage, and an explicit `SALTS_ENOMEM` admission failure
if that allocation fails. Failure before publication releases only local owned
references; callers keep their original slices. Successful publication transfers
immutable packed storage to the existing mailbox, then CNet retains it through
native send settlement. Queue rejection, stale generation and shutdown use the
existing command cleanup. No per-connection buffer, extra mailbox or timer is
introduced; command capacity and payload-byte limits still bound retained work.

MQTT batching remains on the endpoint owner: Receive Maximum, expiry, FIFO and
terminal-packet boundaries are unchanged. A lone flowie/control reply is submitted
immediately; there is no deliberate aggregation delay or flowie/control-packet reorder.
TCP socket defaults, UDP/KCP and WS/WSS behavior are unchanged. The strategy
changes neither public ABI nor flowie/protocol configuration and needs no data
migration. It can be rolled back by removing the private coalescing call while
retaining the same ownership/admission contract. Tests cover packed and mixed
bytes, nonzero offsets, copy limits, caller lifetime, flowie/transport delivery and
close/drain. Throughput and tail-latency improvements require separate measured
qualification; fewer ranges alone are not a performance result.

The historical Windows Release `benchmark_flowie_send_batch` measured retained-vector preparation
for 32 ranges. A 10,000-sample run measured 0.332 vs 0.457 microseconds for 2-byte ranges
and 0.294 vs 0.534 microseconds for 64-byte ranges. The comparison excludes native and TLS
work. The source remains in [transport tests](../flowie/transport/tests), while the current
CTest registers `test_flowie_send_batch` for correctness.

That historical run passed 46 of 49 enabled tests. Three GmSSL handshake suites also failed
on its unchanged `bd84cac` baseline. Its Debug/ASan configuration was blocked by the
`unofficial-lua` dependency. These results describe that build and SDK combination.

Shared bounded stream primitives provide a
demand-grown, single-owner framing accumulator. A private Flowie connection ingress handles
fragmented/sticky MQTT packets on the connection lane, transfers each complete wire packet to
owned message storage, attaches a process-local `{owner_instance_id, session_id, generation}`
route, and publishes each application packet once to the configured TurboFlow source. In
external-session mode CONNECT follows that path; in managed mode it is consumed before graph
admission. The route survives owned
message clone/worker boundaries but is never durable. Reply stages emit encoded flowie/control packets
to the same endpoint adapter; an owner-lane TurboUtils hash map resolves the route in O(1), and
the connection handler does not leave network ownership while an active send can still use its
socket. Focused tests cover direct and worker-Disruptor graph plans. A private,
single-owner session runtime now
copies client identity, fences reconnects by generation, applies complete SUBSCRIBE and
UNSUBSCRIBE packets atomically, bounds inbound inflight records, and can advance QoS 1/2 only
after a selected received/accepted/processed/durable settlement. It produces
PUBACK/PUBREC/PUBCOMP intents but performs no
I/O; the intent now maps directly to the pure flowie/control packet encoder. Persistent QoS 2 release
state survives reconnect while unsettled graph attempts are discarded for explicit redelivery.
The session owner also produces one deterministic CONNECT decision containing acceptance,
session-present, generation-fenced route, CONNACK command, and close-after-rejection policy.
With explicit `manage_sessions: true`, the endpoint owns a bounded client-id hash index of these
session owners. Focused real-TCP tests cover persistent reconnect, session-present,
active-client duplicate rejection, route rebinding, atomic SUBACK/UNSUBACK, PINGRESP,
QoS1 PUBACK, QoS2 PUBREC/PUBCOMP, duplicate suppression, unknown PUBREL, enhanced AUTH/re-auth,
password-provider CONNECT authentication, CONNECT/PUBLISH/SUBSCRIBE authorization denial,
close-after-reply, wildcard/no-local fan-out, shared-subscription round robin, unsubscribe
withdrawal, broker-owned outbound packet IDs, and QoS 2 subscriber reconnect/replay. Only admitted
owned PUBLISH packets cross into the configured worker
Disruptor stage. ProtocolAggregate is the public management resource for MQTT session state. Its
owner-native Status schema v2 reports flowie/transport, session load/capacity, retained count/capacity, persistence/security
binding state, lifecycle, and admission state without exposing client IDs, topics, credentials, or
payload. QUIESCE and RESUME execute on the endpoint owner lane. QUIESCE keeps the OS listener bound
and preserves established MQTT connections/sessions, but closes MQTT admission for newly accepted
connections; RESUME reopens that admission. A command changes generation exactly once, repeated
idempotency keys replay the prior result, and stale generation/deadline/concurrent commands fail
before state mutation. Individual session owners are intentionally not registered as `Connection`
resources because they are flowie/protocol state, not flowie/transport connections.

The same bidirectional endpoint adapter can be named as an explicit graph sink. For a complete
PUBLISH it submits one bounded fan-out command; the graph lane never reads mutable session state.
After a session-owner commit, the endpoint owner lane incrementally updates a derived subscription
selector, matches each unique filter, merges overlapping ordinary subscriptions to one delivery per
session, and selects one active member per exact shared filter with an independent round-robin
cursor. Startup, clean-start replacement, and an explicitly invalid selector use an atomic full
repair from session owners. Session owners remain the only subscription fact source; any incremental
update failure invalidates the derived state, and the next fan-out must complete that repair before
using membership. A failed repair fails fan-out instead of reading a partial selector. No match is a
successful MQTT publish, not `TURBO_ENOTCONN`.

Candidate iteration and live-route comparison use the flowie/protocol-neutral pattern core in
`turbo_flow_protocol`; Flowie does not include an FMQ frame, peer type, or wire enum. The MQTT topic
trie, wildcard rules, shared groups, CRoaring membership, `no_local`, QoS and session generation
remain Flowie-owned inputs to that core. The core therefore cannot mutate subscription truth or
turn MQTT no-match semantics into FMQ's `TURBO_ENOTCONN` behavior.

MQTT Will state is owned by the same session owner. CONNECT deep-copies the Will topic, payload,
properties, QoS, retain bit, and delay into the bounded session record. A normal DISCONNECT reason
suppresses it; abnormal flowie/transport close and DISCONNECT `0x04` make it pending. The owner schedules
publication at the Will Delay boundary or session end, whichever is earlier, and a reconnect with
the same client id cancels a still-pending Will before fan-out. The generated owned MQTT PUBLISH
enters the configured TurboFlow graph with a pointer-free internal flag and route token; an endpoint
sink transfers the graph-transformed packet into the existing bounded owner command queue before
the committed Will record is cleared. Cluster mode restores the remaining absolute delay/expiry
boundary from the TurboRaft state-machine snapshot and committed log. Standalone `:memory:` ORM
state intentionally cannot restore a Will after process restart. A failed
graph or repository operation keeps the Will pending and schedules a bounded retry. The committed
TurboRaft log and snapshot provide at-least-once server/cluster recovery, not exactly-once publication: a crash after owner-command
admission but before the record clear can repeat the Will after restart.

Each selected delivery is re-encoded for the subscriber MQTT version. Its QoS is
`min(inbound QoS, granted QoS)`; `no_local` and retain-as-published are applied before admission.
MQTT 5 Topic Alias and inbound Subscription Identifier properties are not forwarded because both
belong to a connection/subscription context. The endpoint keeps a bounded per-connection Topic
Alias map: a PUBLISH carrying both a topic and alias updates that map, while a later alias-only
PUBLISH is normalized to a concrete-topic packet before graph publication. Unknown, zero, or
out-of-range aliases close MQTT 5 with Topic Alias invalid (`0x94`). QoS 1/2 deliveries use
broker-owned packet IDs and owner-held outbound inflight records. PUBACK removes QoS 1 state;
PUBREC emits PUBREL; PUBCOMP completes QoS 2. A persistent reconnect replays pending PUBLISH with
DUP set or the pending PUBREL after CONNACK.

Fan-out prebuilds all target packets and atomically reserves their endpoint aggregate budget before
peer admission. Each accepted packet then enters the matching connection's own bounded queue and
same-connection FIFO drain. A subscriber that exhausts its pending-send HWM or MQTT outbound
inflight quota is disconnected and its unsent queue is released; other matching subscribers keep
their deliveries and the publisher is not failed for that peer-local condition. Aggregate/resource
admission failure still fails the fan-out command so QoS 1/2 can redeliver; committed subscriber
inflight state remains eligible for reconnect replay within the store lifetime. Endpoint config ABI v8 and strict YAML expose
this fixed behavior as `slow_subscriber_policy: disconnect`; zero in the C API selects the same
default and any other value fails registration. Queue Status schema v2 keeps the aggregate
`load/capacity` view and additionally reports `connection_hwm_bytes`, the stable policy enum, and a
saturating `slow_subscriber_disconnects` counter. A peer-local overflow increments that counter but
does not mark the endpoint aggregate saturated or stop admission for healthy connections.
The selector uses a topic-level trie for exact, `+`, and `#` candidate pruning. Typed member
metadata stays in a hash index, while a derived CRoaring64 set accelerates integer membership and
rank queries; neither structure owns MQTT payload or session facts. A filter hash resolves
the stable entry slot in expected O(1); entry and member mutations update only that filter. Trie
terminal bindings support O(1) bucket removal, own exact-level tokens, prune empty branches, and
reuse inactive entry slots, so subscribe/unsubscribe churn does not retain removed filter storage.
CRoaring deduplicates overlapping ordinary matches and selects shared members by rank without
copying a candidate array. Unrelated mutations preserve each shared filter's cursor. The trie also
enforces the MQTT `$SYS` root-wildcard boundary before the defensive flowie/protocol matcher.

The standalone endpoint selects one recipient per shared subscription. The endpoint owner prefers
a connected member. If every member is offline, a QoS 1 or 2 delivery can be assigned to one
unexpired persistent session with space in its existing delivery quota. Selection follows the
same round-robin traversal. Once assigned, the session owner holds the encoded packet and Packet
Identifier through reconnect, acknowledgement or expiry. The configured repository commits this
state through the ordinary session-delivery path. The session's expiry, capacity limits and
shutdown rules apply to the queued copy. Offline QoS 0 has no queued copy.

Assigning the message at publish time lets the endpoint reuse session storage, replay and
backpressure. A separate group queue would allow another member to claim the message after
reconnect and would require group-owned quota, expiry and persistence. The session-based policy
requires no schema migration. The previous standalone version can read the same delivery records.

The repository-owned remote load runner at
`deploy/server/tests/run-mqtt-scale-load.sh` is the current executable capacity evidence. Its default
tiers create 96, 192, and 384 simultaneous MQTT 5 clients across QoS 0/1/2 and validate exact payload
counts, multiset equality, per-publisher FIFO, server/listener survival, CPU, RSS/VM, threads, file
descriptors, connection peaks, and bounded diagnostic logs. The 2026-08-24 EU Debug runs passed all
three tiers. These results are correctness and resource evidence for that host/build/workload, not a
portable connection-rate, latency, or production-capacity SLA.

The endpoint derives the private Salts executor capacity from `max_connections` plus fixed
headroom and exposes `coroutine_stack_size` for that executor plus `stream_recv_buffer_bytes` for CNet streams.
Explicit values on borrowed, transferred-owned, or pool-lane contexts fail with `SALTS_ENOTSUP`
because their host owns capacity. Flowie defaults the CNet stream receive buffer to 4 KiB and MQTT
framing reassembles packets incrementally up to `max_packet_size`; TLS independently raises its bounded
BIO storage to CNet's 17 KiB minimum. The private-executor minimum stack is 64 KiB for managed
security and MQTT 5 AUTH state. Explicit 32 KiB configurations must migrate to at least 64 KiB. The remote
runner records peak and settled memory for every tier instead of treating the theoretical pool
capacity as committed memory. Capacity changes must be validated by repeated runs on a stable host;
a single Debug high-water measurement is not a connection- or subscription-rate benchmark.

One owner-lane dispatcher preserves command order up to route admission; it never waits for socket
I/O. Each connection coroutine owns both receive/framing and its bounded FIFO send drain. Enqueue
interrupts a pending receive wait, after which that same owner drains queued packets before arming
receive again. A TCP drain combines 2-64 already-admitted replies into one scatter/gather send;
single replies and non-TCP transports retain the existing one-packet send path. A terminal reply is
always the final vector item, and queue budgets are released only after the send completes. Fan-out
therefore does not transiently allocate a second coroutine per recipient.
Different socket owners can still make progress independently, while one connection keeps strict
FIFO and one mutable flowie/transport owner. Shutdown closes admission, interrupts socket waits, releases
both global and connection budgets, and waits for connection owners before destroying state. The
current remote runner covers active-subscriber fan-out and owner cleanup after each tier. A
stalled-subscriber injection, reconnect/replay workload, and shutdown-under-load phase are explicit
runner extensions; until those cases are implemented and executed, the configured HWM/disconnect
policy remains a code contract rather than current load-test evidence.

The endpoint owner lane also owns a bounded exact-topic retained table. A retained PUBLISH replaces
the previous packet only after the complete graph command reaches the configured Flowie fan-out
sink; a zero-length payload removes it, and an expired MQTT 5 Message Expiry Interval removes it
before subscription replay. SUBSCRIBE captures whether each filter existed before the atomic
session mutation, sends SUBACK first, then applies Retain Handling 0/1/2. Shared subscriptions never
receive retained replay. Replayed QoS 1/2 packets use the same broker packet-id allocator, session
delivery state, flowie/protocol-store session CAS, and bounded reply Queue as live fan-out. Retained capacity is
independently bounded by `max_retained_messages` (zero in the C API selects `max_sessions`). When a
`protocol_store` is bound, retained PUT/replace/delete crosses the atomic ORM repository CAS boundary
before the owner changes its in-memory fact. A reserved binary key prefix separates canonical
versioned `FRET` records from the existing client-id keyed session records. Startup restores valid
non-expired retained records and CAS-deletes expired records before opening the listener.

Finite MQTT 5 session expiry is scheduled on the same endpoint owner lane with a reusable wait.
Disconnect records a monotonic deadline fenced by session generation; reconnect clears the old
deadline, and expiry removes the inactive session and its selector memberships on the owner lane.
An already-invalid selector is left invalid for the next atomic repair. A zero interval removes
state immediately, while MQTT 3.1 and MQTT 3.1.1 persistent sessions retain
their unbounded in-process lifetime. MQTT 5 DISCONNECT Session Expiry Interval overrides are
validated by the session owner before close, including the rule that a zero CONNECT interval cannot
be changed to a non-zero value. The live timer remains process-local, while its record carries an
absolute wall-clock expiry. TurboRaft's committed log and snapshot preserve that bound in server/cluster
mode; standalone SQLite `:memory:` discards the record at process exit. The typed standalone
repository provides bounded namespace scan, per-record revision CAS, and serializable atomic commit
through `Orm::C`. The internal session codec emits canonical versioned LTV and deliberately excludes
live routes, credentials, reserved outbound identifiers, and unsettled graph attempts. Decode
always creates an inactive owner under the new endpoint instance. The additive
`flowie_endpoint_bindings_t` injects a borrowed store without extending endpoint config ABI;
resolved YAML must name the same channel with `protocol_store` (or legacy `session_store`).
Registration scans and validates the
namespace, deletes expired records with revision CAS, rebuilds the trie/bitmap selector, and
advances the local session-id allocator before the listener starts. CONNECT, SUBSCRIBE,
UNSUBSCRIBE, inbound QoS transitions, outbound delivery transitions, disconnect and close use
clone -> atomic CAS commit -> owner swap. CONNACK, SUBACK, UNSUBACK and QoS ACK/socket sends occur
only after the relevant commit. Principal identity, roles and groups are encoded field-by-field;
credentials, live routes and unsettled graph attempts are never stored. Provider-neutral fault
tests cover commit/recovery semantics; live backends verify their own restart behavior.

Secure PUBLISH checks one concrete Topic Name before session inflight admission. Secure SUBSCRIBE
checks every requested Topic Filter before the atomic session mutation; adapter-match ACL rules use
filter-language containment, not string matching, so a policy for `root-a/#` does not authorize a
broader `#`. MQTT 5 authentication failure maps to CONNACK `0x86`, authorization failure to `0x87`,
and unauthorized QoS 1/2 publication or subscription returns per-packet `0x87` without graph work.
The MQTT packet parser validates these Topic Name and Topic Filter spans before the endpoint copies
them into its session-owned authorization scratch string. That exact copy carries an internal,
single-call provenance token and length into the immutable matcher, avoiding duplicate UTF-8 and
wildcard scans. The token type is not installed or exposed as public API. Programmatic matcher calls
without this provenance still perform full validation; truncated contexts, unknown resource kinds,
or forged provenance fail with a flowie/protocol error instead of falling back to the fast path. Both paths
share the same trie traversal, `$SYS` boundary, filter-containment semantics, rule ordering, and deny
precedence.
MQTT 3.1.1 uses its available CONNACK/SUBACK failures and closes when the flowie/protocol has no negative
publish ACK. MQTT 3.1 uses legacy CONNACK authentication failures, but closes on an authorization
or capacity failure that cannot be represented by its successful-only SUBACK/PUBACK vocabulary.
MQTT 5 Authentication Method selects the configured enhanced provider without basic
authentication fallback. Initial challenge exchange uses AUTH `0x18`, returns the final method/data
in CONNACK, and connected re-authentication starts with AUTH `0x19`. Re-authentication may refresh
roles, expiry, and policy version only for the same principal/type/domain session owner; an owner
change or CONNECT re-authorization failure closes the connection.

These ingress/session owners remain internal and their headers are not installed. The endpoint
now has a tested graph-to-socket flowie/control reply path, but it is not yet a complete MQTT broker.
The flowie/transport contract runs real MQTT 3.1, MQTT 3.1.1, and MQTT 5 CONNECT, PING, and shutdown
traffic over TCP, TLS, WS, and WSS. TLS/WSS use a verified test CA and certificate; a
successful config projection is not accepted as flowie/transport evidence.
The standalone server assembly creates the configured TurboDB repository and injects the
borrowed persistence binding into the endpoint. It closes the endpoint before closing the
repository. `--check` assembles this graph of components and validates the selected record
store without starting the MQTT listener. The independent TurboFlow host builds and validates
its own Graph. Experimental cluster composition creates `TurboRaft::Service` and
`TurboRaft::WalStorage` for cluster facts.
Authentication providers are selected from `profiles.<name>.auth_provider` and the referenced
`kind: auth_provider` channel. The worker reads `config.backend`, selects exactly one factory
registered by the product composition root, and fails before listener startup when the backend is
missing, duplicated, unavailable, or malformed. Flowie owns only the generic provider lifecycle
envelope and authentication ABI. The bundled product registers only the `https` authentication
backend. Redis, SQLite, PostgreSQL, and any other credential database remain private implementation
details of that service and cannot be configured through an `auth_provider` channel. When
`flowie-control` bridges an enterprise identity system, its sole credential source is the configured
third-party HTTPS assertion service; its Repository contributes only local enabled/Role/Group/ACL
authorization facts. TurboDB ORM, the flowie/control Repository, and Graph adapters never form a fallback
authentication chain. The CONNECT
request invokes CHTTP directly; DNS, TCP, TLS, send, and receive are bounded by the CHTTP client
runtime. Redirects and retries are disabled, request/response
sizes and timeouts are bounded, the service token is acquired by reference for every request, and
all flowie/transport, certificate, status, content-type, version, or principal-validation failures deny
authentication without a database or anonymous fallback. The returned principal is then evaluated
by the local SecurityRealm; publish/subscribe ACL checks never perform an HTTP or database call.
See [HTTPS authentication](ADR_HTTPS_AUTH_SERVICE.md) for trust and
[server storage configuration](SERVER_GUIDE.md#sessionretained-与持久化) for runtime options.

### ACL management plane

`flowie/control` builds the installable `flowie-control` and `flowie-control-data` tools. Its TurboDB-backed
flowie/control store is the single fact source for root-scoped users, credentials, hierarchical groups,
roles, subject-keyed ACL draft rules and audit records. Draft rules are canonical subject-scoped
documents. A publish command runs under one
TurboDB transaction, revalidates the complete draft and every referenced principal,
group, role and MQTT adapter filter, then atomically replaces the provider-compatible
`flowie_control_published_bundle`/`flowie_control_published_rule` generation. Store revision and policy version
advance independently. Failed or stale publishes leave the previous provider snapshot unchanged.

The management service is the only interface used above the store. It enforces the root/actor
identity supplied by a trusted flowie/transport adapter and the `viewer`, `user_admin`, `policy_admin` and
`security_admin` permission matrix. Read APIs use bounded keyset pages; no HTTP or Dashboard handler
receives a database handle.

The Chttp App adapter exposes the bounded JSON-RPC methods in the
[management API reference](MANAGEMENT_RPC_API.md). Introspection, batch and notifications
are disabled. Actor and Domain come from trusted session context. The server-rendered
Dashboard uses the same service, local HTMX assets, CSRF validation, security headers and
escaping. The App must finish shutdown before runtime dependencies are released.

The managed runtime supports explicit `received`, `accepted`, `processed`, and `durable` QoS
settlement policies. `received` advances the session owner before graph admission. `processed` advances it
only after the synchronous TurboFlow publication returns successfully, including configured
worker-stage completion. `accepted` requires the selected graph admission stage to consume the
one-shot settlement envelope explicitly and route the settlement command back to the Flowie owner.
That callback only enters the endpoint's bounded reply command queue; the endpoint owner lane still
owns session mutation and socket send. An arbitrary successful stage is not an ACCEPTED boundary.
The current endpoint config exposes this typed policy and intentionally rejects every non-current
layout.

`durable` is emitted only by an explicitly configured Graph storage primitive after its own commit
boundary. The sink may use the current live, generation-fenced route to enqueue the owner settlement
command after commit, but that route is never serialized. Such a Graph sink is an external side
effect: it neither restores standalone flowie/protocol facts nor becomes a second server/cluster fact source.
Whole-graph success is not evidence of durable commit, especially with branching.

The current Flowie composition evaluates Rules directly after endpoint admission. A different
`.flow` may connect a durable store before Rules when the original admitted packet is the fact
source, or after Rules when the transformed message is the fact source; the order is explicit and
not inferred. Ingress stores the negotiated MQTT version and the packet's fixed-header flags in
private message metadata; the complete wire packet remains the
message-owned bytes in the retained buffer. An internal Flowie facts provider uses that metadata and the
existing typed parser to materialize a 15-field bounded schema. The base fields are `mqtt.topic`,
`mqtt.payload`, `mqtt.payload_size`, `mqtt.qos`, `mqtt.retain`, `mqtt.duplicate`, `mqtt.packet_id`,
and `mqtt.version`. MQTT 5 adds nullable `mqtt.payload_format_indicator`,
`mqtt.message_expiry_interval`, `mqtt.content_type`, `mqtt.response_topic`, and binary-safe
`mqtt.correlation_data`; `mqtt.user_property_count` reports the bounded repeatable-property count,
and `mqtt.broker_will` identifies a broker-generated Will. String facts are borrowed
length-delimited views valid only for the current `rules.apply` call. Optional properties use NULL
when absent rather than a fabricated zero or empty value. No parsed projection, bitmap, parser
owner, live connection route, or duplicated payload is persisted. Store replay reparses the
authoritative wire payload and rebuilds the projection. The version bits are Flowie flowie/protocol
metadata and must not be overwritten before a later MQTT facts evaluation; current transforms
mutate status instead.

The composition tests exercise multiple concrete graphs. Endpoint admission feeds `rules.apply`,
which mutates private status and routes each MQTT PUBLISH either to the existing Flowie endpoint
fan-out sink or to the existing CNet TCP socket sink. Provider-neutral ORM repository tests exercise
commit faults and recovery independently. The product host resolves a RuleSet
program only when configured, while the Graph explicitly names every CNet, CHTTP, Redis, or PostgreSQL
source/sink adapter. The host does not infer routes, facts providers, storage destinations, or
unregistered adapter kinds.

An external durable Graph point proves only that sink's message persistence, not broker recovery or
exactly-once delivery. Standalone flowie/protocol facts are restored only from its ORM repository. Cluster
owner/PUBLISH data is restored only by replaying the TurboRaft committed log and snapshot; Graph
sinks cannot feed that state machine during recovery. A crash after an external sink commit but
before PUBACK can still cause the client to redeliver an already stored PUBLISH.
