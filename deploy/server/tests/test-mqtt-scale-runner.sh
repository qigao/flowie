#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNNER="$SCRIPT_DIR/run-mqtt-scale-load.sh"
TEST_ROOT=$(mktemp -d)
SERVER_PID=

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT HUP INT TERM

LIVE_LOG="$TEST_ROOT/live.log"
STALE_LOG="$TEST_ROOT/stale.log"
: > "$LIVE_LOG"
: > "$STALE_LOG"

OUTPUT=$(bash "$RUNNER" --plan --tiers 1 --messages 1 --payload-bytes 100)
grep -F \
  'capacity-plan tiers=1 messages_per_publisher=1 max_clients=6 required_inflight_per_session=1 payload_bytes=100 expected_delivery_payload_bytes=300' \
  <<< "$OUTPUT"

OUTPUT=$(bash "$RUNNER" --plan --tiers 1 --messages 1 --payload-bytes 4096)
grep -F \
  'capacity-plan tiers=1 messages_per_publisher=1 max_clients=6 required_inflight_per_session=1 payload_bytes=4096 expected_delivery_payload_bytes=12288' \
  <<< "$OUTPUT"

for invalid_payload_bytes in 99 4097; do
  set +e
  OUTPUT=$(bash "$RUNNER" --plan --tiers 1 --messages 1 \
    --payload-bytes "$invalid_payload_bytes" 2>&1)
  STATUS=$?
  set -e
  if ((STATUS == 0)); then
    printf 'runner unexpectedly accepted payload bytes: %s\n' "$invalid_payload_bytes" >&2
    exit 1
  fi
  grep -F 'payload-bytes must be between 100 and 4096' <<< "$OUTPUT"
done

set +e
OUTPUT=$(bash "$RUNNER" --plan --tiers 1000000000 --messages 1 \
  --payload-bytes 4096 2>&1)
STATUS=$?
set -e
if ((STATUS == 0)); then
  printf 'runner unexpectedly accepted overflowing delivery payload bytes\n' >&2
  exit 1
fi
grep -F 'expected-delivery-payload-bytes exceeds signed 64-bit capacity' <<< "$OUTPUT"

bash -c 'exec -a flowie_server sleep 300' > "$LIVE_LOG" 2>&1 &
SERVER_PID=$!

set +e
OUTPUT=$(bash "$RUNNER" \
  --server-pid "$SERVER_PID" \
  --server-log "$STALE_LOG" \
  --artifacts "$TEST_ROOT/artifacts" \
  --max-connections 6 \
  --max-inflight 1 \
  --tiers 1 \
  --messages 1 2>&1)
STATUS=$?
set -e

if ((STATUS == 0)); then
  printf 'runner unexpectedly accepted a detached server log\n' >&2
  exit 1
fi
grep -F "server log does not match stdout/stderr for PID $SERVER_PID" <<< "$OUTPUT"
[[ ! -e "$TEST_ROOT/artifacts" ]]

printf 'mqtt scale runner log attachment contract: PASS\n'
