#!/usr/bin/env python3
from pathlib import Path

source = Path("flowie/src/flowie_endpoint.c").read_text(encoding="utf-8")

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
    raise SystemExit("Task 2 must still initialize exactly one runtime owner lane")

print(
    "Flowie endpoint owner-lane boundary verified: runtime-owner identity is array-backed, "
    "lane 0 preserves current execution/server behavior, and transport/connection/session "
    "ownership remains lane-scoped."
)
