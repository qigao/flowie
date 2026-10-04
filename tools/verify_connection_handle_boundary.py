#!/usr/bin/env python3
from pathlib import Path

header = Path("connection/include/flowie_connection.h").read_text(encoding="utf-8")
source = Path("connection/src/flowie_connection.c").read_text(encoding="utf-8")

expected_public = """typedef struct flowie_connection {
  uint32_t slot;
  uint32_t generation;
} flowie_connection;"""
if expected_public not in header:
    raise SystemExit("public flowie_connection ABI changed from {slot,generation}")
if "uint32_t owner;" in header:
    raise SystemExit("public flowie_connection exposed a runtime owner/shard token")

required = {
    "peer generation": "uint32_t generation;",
    "global peer-table handle": "return (flowie_connection){(uint32_t)(index + 1u), peer->generation};",
    "O(1) peer lookup": "index = (size_t)connection.slot - 1u;",
    "generation lookup guard": "peer->used && peer->generation == connection.generation",
    "accepted generation publish": "++peer->generation;",
    "state stale callback guard": "peer->connection.slot != connection.slot",
    "state generation guard": "peer->connection.generation != connection.generation",
}
for label, marker in required.items():
    if marker not in source:
        raise SystemExit(f"missing stream-handle boundary: {label}: {marker}")

for forbidden in (
    "static flowie_connection flowie_stream_handle(cnet_connection connection)",
    "(flowie_connection){connection.slot, connection.generation}",
    "(flowie_connection){connection.slot,connection.generation}",
):
    if forbidden in source:
        raise SystemExit(
            f"public stream handle regressed to owner-local CNet identity: {forbidden}"
        )

if source.count("++peer->generation;") != 1:
    raise SystemExit("stream peer generation must advance exactly at successful accept publication")

print(
    "Flowie stream handle boundary verified: public {slot,generation} is Flowie-global, "
    "CNet slot/generation remains internal, and stale transport callbacks are generation-checked."
)
