#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ENTRYPOINT="$SCRIPT_DIR/../flowie-combined-entrypoint.sh"
TEST_ROOT=$(mktemp -d)
COMBINED_PID=

cleanup() {
  if [ -n "$COMBINED_PID" ] && kill -0 "$COMBINED_PID" 2>/dev/null; then
    kill -TERM "$COMBINED_PID" 2>/dev/null || true
    wait "$COMBINED_PID" 2>/dev/null || true
  fi
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT HUP INT TERM

wait_for_file() {
  wait_count=0
  while [ ! -e "$1" ] && [ "$wait_count" -lt 100 ]; do
    sleep 0.05
    wait_count=$((wait_count + 1))
  done
  test -e "$1"
}

cat >"$TEST_ROOT/server-entrypoint" <<'EOF'
#!/bin/sh
printf '%s' "${FLOWIE_CHECK:-}" >"$FLOWIE_COMBINED_TEST_ROOT/server-check"
if [ "${FLOWIE_COMBINED_TEST_MODE:-}" = check ]; then
  exit 0
fi
printf '%s\n' started >"$FLOWIE_COMBINED_TEST_ROOT/server-started"
trap 'printf "%s\n" TERM >"$FLOWIE_COMBINED_TEST_ROOT/server-signal"; exit 0' TERM
trap 'printf "%s\n" INT >"$FLOWIE_COMBINED_TEST_ROOT/server-signal"; exit 0' INT
trap 'printf "%s\n" HUP >"$FLOWIE_COMBINED_TEST_ROOT/server-signal"; exit 0' HUP
while :; do sleep 0.1; done
EOF
chmod +x "$TEST_ROOT/server-entrypoint"

cat >"$TEST_ROOT/control-entrypoint" <<'EOF'
#!/bin/sh
printf '%s' "${FLOWIE_CONTROL_CHECK:-}" >"$FLOWIE_COMBINED_TEST_ROOT/control-check"
if [ "${FLOWIE_COMBINED_TEST_MODE:-}" = check ]; then
  exit 0
fi
printf '%s\n' started >"$FLOWIE_COMBINED_TEST_ROOT/control-started"
if [ "${FLOWIE_COMBINED_TEST_MODE:-}" = control-fails ]; then
  while [ ! -e "$FLOWIE_COMBINED_TEST_ROOT/server-started" ]; do sleep 0.05; done
  exit 23
fi
trap 'printf "%s\n" TERM >"$FLOWIE_COMBINED_TEST_ROOT/control-signal"; exit 0' TERM
trap 'printf "%s\n" INT >"$FLOWIE_COMBINED_TEST_ROOT/control-signal"; exit 0' INT
trap 'printf "%s\n" HUP >"$FLOWIE_COMBINED_TEST_ROOT/control-signal"; exit 0' HUP
while :; do sleep 0.1; done
EOF
chmod +x "$TEST_ROOT/control-entrypoint"

FLOWIE_SERVER_ENTRYPOINT="$TEST_ROOT/server-entrypoint" \
FLOWIE_CONTROL_ENTRYPOINT="$TEST_ROOT/control-entrypoint" \
FLOWIE_COMBINED_TEST_ROOT="$TEST_ROOT" \
FLOWIE_COMBINED_TEST_MODE=check \
FLOWIE_COMBINED_CHECK=yes \
  sh "$ENTRYPOINT"
test "$(sed -n '1p' "$TEST_ROOT/server-check")" = 1
test "$(sed -n '1p' "$TEST_ROOT/control-check")" = 1
test ! -e "$TEST_ROOT/server-started"
test ! -e "$TEST_ROOT/control-started"

FLOWIE_COMBINED_TEST_OUTPUT="$TEST_ROOT/passthrough" \
  sh "$ENTRYPOINT" sh -c 'printf "%s" passed >"$FLOWIE_COMBINED_TEST_OUTPUT"'
test "$(sed -n '1p' "$TEST_ROOT/passthrough")" = passed

rm -f "$TEST_ROOT/server-check" "$TEST_ROOT/control-check"
set +e
FLOWIE_SERVER_ENTRYPOINT="$TEST_ROOT/server-entrypoint" \
FLOWIE_CONTROL_ENTRYPOINT="$TEST_ROOT/control-entrypoint" \
FLOWIE_COMBINED_TEST_ROOT="$TEST_ROOT" \
FLOWIE_COMBINED_TEST_MODE=control-fails \
  sh "$ENTRYPOINT"
failure_status=$?
set -e
test "$failure_status" -eq 23
test "$(sed -n '1p' "$TEST_ROOT/server-signal")" = TERM

rm -f "$TEST_ROOT/server-started" "$TEST_ROOT/control-started" \
  "$TEST_ROOT/server-signal" "$TEST_ROOT/control-signal"
FLOWIE_SERVER_ENTRYPOINT="$TEST_ROOT/server-entrypoint" \
FLOWIE_CONTROL_ENTRYPOINT="$TEST_ROOT/control-entrypoint" \
FLOWIE_COMBINED_TEST_ROOT="$TEST_ROOT" \
FLOWIE_COMBINED_TEST_MODE=running \
  sh "$ENTRYPOINT" &
COMBINED_PID=$!
wait_for_file "$TEST_ROOT/server-started"
wait_for_file "$TEST_ROOT/control-started"
kill -TERM "$COMBINED_PID"
set +e
wait "$COMBINED_PID"
signal_status=$?
set -e
COMBINED_PID=
test "$signal_status" -eq 143
test "$(sed -n '1p' "$TEST_ROOT/server-signal")" = TERM
test "$(sed -n '1p' "$TEST_ROOT/control-signal")" = TERM

if FLOWIE_SERVER_ENTRYPOINT="$TEST_ROOT/server-entrypoint" \
  FLOWIE_CONTROL_ENTRYPOINT="$TEST_ROOT/control-entrypoint" \
  FLOWIE_COMBINED_TEST_ROOT="$TEST_ROOT" \
  FLOWIE_COMBINED_CHECK=maybe \
  sh "$ENTRYPOINT"; then
  echo "flowie-combined entrypoint accepted invalid FLOWIE_COMBINED_CHECK" >&2
  exit 1
fi

echo "flowie-combined-entrypoint contract: PASS"
