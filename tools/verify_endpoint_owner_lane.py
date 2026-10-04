#!/usr/bin/env python3
from pathlib import Path

source = Path("flowie/src/flowie_endpoint.c").read_text(encoding="utf-8")

required = {
    "owner lane type": "struct flowie_endpoint_owner_lane_s {",
    "connection runtime owner": "flowie_endpoint_owner_lane_t *runtime_owner;",
    "endpoint owner lane": "flowie_endpoint_owner_lane_t owner_lane;",
    "single-lane count": "endpoint->owner_lane_count = 1u;",
    "observer lane user": "flowie_net_close, NULL, runtime_owner",
    "connection lane admission": "connection->runtime_owner = runtime_owner;",
    "session bind lane guard": "session->runtime_owner != connection->runtime_owner",
    "callback owner validation": "flowie_owner_lane_valid(call->runtime_owner)",
}
for label, marker in required.items():
    if marker not in source:
        raise SystemExit(f"missing owner-lane contract: {label}: {marker}")

if "flowie_net_close, NULL,\n                                      endpoint" in source:
    raise SystemExit("transport observer still uses endpoint as callback owner")

if "flowie_endpoint_t *endpoint = (flowie_endpoint_t *)user;" in source:
    raise SystemExit("transport callback still recovers endpoint directly from observer user")

if source.count("owner_lane_count = 1u") != 1:
    raise SystemExit("Task 1 must initialize exactly one runtime owner lane")

print("Flowie endpoint owner-lane boundary verified: one explicit runtime lane; transport observer, connection and session identity are lane-scoped.")
