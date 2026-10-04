#!/usr/bin/env python3
from pathlib import Path

source = Path("flowie/src/flowie_endpoint.c").read_text(encoding="utf-8")
connection_header = Path("connection/include/flowie_connection.h").read_text(
    encoding="utf-8"
)

required = {
    "owner lane type": "struct flowie_endpoint_owner_lane_s {",
    "connection runtime owner": "flowie_endpoint_owner_lane_t *runtime_owner;",
    "endpoint owner lane array": "flowie_endpoint_owner_lane_t *owner_lanes;",
    "single-lane allocation": "endpoint->owner_lanes = (flowie_endpoint_owner_lane_t *)calloc(",
    "single-lane count": "endpoint->owner_lane_count = 1u;",
    "primary owner index": "owner = &endpoint->owner_lanes[0];",
    "array membership validation": "owner >= endpoint->owner_lanes + endpoint->owner_lane_count",
    "observer lane user": "flowie_net_close, NULL, runtime_owner",
    "connection lane admission": "connection->runtime_owner = runtime_owner;",
    "session bind lane guard": "session->runtime_owner != connection->runtime_owner",
    "callback owner validation": "flowie_owner_lane_valid(call->runtime_owner)",
    "lane owned execution storage": "tf_execution_t execution_storage;",
    "lane owned server storage": "flowie_server server_storage;",
    "lane execution self pointer": ".execution = &endpoint->owner_lanes[0].execution_storage,",
    "lane server self pointer": ".server = &endpoint->owner_lanes[0].server_storage,",
    "connection execution helper": "flowie_connection_execution(connection)",
    "connection server helper": "flowie_connection_server(connection)",
    "lane storage release": "free(endpoint->owner_lanes);",
}
for label, marker in required.items():
    if marker not in source:
        raise SystemExit(f"missing owner-lane contract: {label}: {marker}")

for forbidden in (
    "flowie_endpoint_owner_lane_t owner_lane;",
    "return &endpoint->owner_lane;",
    "owner == &owner->endpoint->owner_lane",
):
    if forbidden in source:
        raise SystemExit(f"endpoint runtime owner regressed to singleton storage: {forbidden}")

if "flowie_net_close, NULL,\n                                      endpoint" in source:
    raise SystemExit("transport observer still uses endpoint as callback owner")

if "flowie_endpoint_t *endpoint = (flowie_endpoint_t *)user;" in source:
    raise SystemExit("transport callback still recovers endpoint directly from observer user")

if source.count("endpoint->owner_lane_count = 1u;") != 1:
    raise SystemExit("storage prerequisite must still initialize exactly one runtime owner lane")

for forbidden in (
    "tf_execution_t execution;\n  flowie_server server;",
    "endpoint->execution",
    "endpoint->server",
    "connection->endpoint->execution",
    "connection->endpoint->server",
):
    if forbidden in source:
        raise SystemExit(f"runtime storage escaped owner lane: {forbidden}")

if "uint32_t owner;" in connection_header:
    raise SystemExit(
        "public flowie_connection must not expose a runtime-owner/shard token"
    )

if "typedef struct flowie_connection {\n  uint32_t slot;\n  uint32_t generation;\n}" not in connection_header:
    raise SystemExit("public flowie_connection ABI unexpectedly changed")

print(
    "Flowie endpoint owner-lane boundary verified: runtime owner storage is lane-owned, "
    "lane 0 preserves current behavior, connection-scoped runtime calls stay on the bound "
    "lane, and the public flowie_connection ABI exposes no owner/shard token."
)
