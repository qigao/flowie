#!/usr/bin/env python3
from pathlib import Path

source = Path("connection/src/flowie_connection.c").read_text(encoding="utf-8")
header = Path("connection/include/flowie_connection.h").read_text(encoding="utf-8")

owner_begin = source.index("struct flowie_stream_owner_lane {")
owner_end = source.index("};", owner_begin) + 2
owner_block = source[owner_begin:owner_end]

server_begin = source.index("typedef struct flowie_server_impl {")
server_end = source.index("} flowie_server_impl;", server_begin) + len("} flowie_server_impl;")
server_block = source[server_begin:server_end]

required_owner = (
    "struct flowie_server_impl *server;",
    "cnet_client stream;",
    "flowie_command *commands;",
    "salts_mutex_t mutex;",
    "salts_thread_t thread;",
    "bool stream_initialized;",
    "bool thread_started;",
    "bool worker_done;",
)
for marker in required_owner:
    if marker not in owner_block:
        raise SystemExit(f"stream owner lane missing runtime state: {marker}")

required_server = (
    "flowie_stream_owner_lane *stream_owners;",
    "size_t stream_owner_count;",
    "cnet_listener listener;",
    "cnet_tls_server tls;",
    "cnet_packet_endpoint packet;",
    "chttp_server websocket;",
    "flowie_stream_peer *stream_peers;",
)
for marker in required_server:
    if marker not in server_block:
        raise SystemExit(f"server missing expected shared/control state: {marker}")

for forbidden in (
    "cnet_client stream;",
    "bool stream_initialized;",
):
    if forbidden in server_block:
        raise SystemExit(f"stream runtime escaped owner lane: {forbidden}")

required_source = {
    "single lane allocation": "server->stream_owner_count = 1u;",
    "primary owner helper": "flowie_stream_primary_owner(",
    "peer runtime owner": "flowie_stream_owner_lane *runtime_owner;",
    "peer callback client": "flowie_stream_peer_client(peer)",
    "peer terminal owner clear": "peer->runtime_owner = NULL;",
    "owner command progress": "flowie_stream_command_progress(flowie_stream_owner_lane *owner)",
    "owner command guard": "peer->runtime_owner != owner",
    "owner worker": "flowie_stream_worker(void *user)",
    "owner worker create": "salts_thread_create(&owner->thread, flowie_stream_worker, owner)",
    "owner wake": "cnet_client_wake(&owner->stream)",
    "owner poll": "cnet_client_poll(&owner->stream",
    "packet worker preserved": "flowie_packet_worker(void *user)",
    "packet mailbox preserved": "flowie_packet_command_progress(flowie_server_impl *server)",
}
for label, marker in required_source.items():
    if marker not in source:
        raise SystemExit(f"missing stream owner-runtime contract: {label}: {marker}")

for forbidden in (
    "cnet_client_wake(&server->stream",
    "cnet_client_wake(&impl->stream",
    "cnet_client_poll(&server->stream",
    "cnet_client_poll(&impl->stream",
    "cnet_client_stop(&server->stream",
    "cnet_client_stop(&impl->stream",
    "cnet_client_destroy(&server->stream",
    "cnet_client_destroy(&impl->stream",
):
    if forbidden in source:
        raise SystemExit(f"stream CNet runtime regressed to server singleton: {forbidden}")

if source.count("server->stream_owner_count = 1u;") != 1:
    raise SystemExit("storage-only prerequisite must initialize exactly one stream owner lane")

config_begin = header.index("typedef struct flowie_server_config {")
config_end = header.index("} flowie_server_config;", config_begin)
config_block = header[config_begin:config_end]
if "owner_count" in config_block:
    raise SystemExit("storage-only prerequisite must not expose stream owner_count yet")

public_handle = (
    "typedef struct flowie_connection {\n"
    "  uint32_t slot;\n"
    "  uint32_t generation;\n"
    "} flowie_connection;"
)
if public_handle not in header or "uint32_t owner;" in header:
    raise SystemExit("public flowie_connection ABI must remain {slot,generation}")

print(
    "Flowie stream runtime boundary verified: one internal owner lane owns the "
    "CNet client, bounded stream command mailbox, worker thread and lifecycle; "
    "listener/TLS/global public-handle table remain server-owned, packet runtime "
    "is unchanged, and no public owner token or owner_count API is exposed."
)
