#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HEALTHCHECK="$SCRIPT_DIR/../healthcheck.sh"
TEST_ROOT=$(mktemp -d)
trap 'rm -rf "$TEST_ROOT"' EXIT HUP INT TERM

run_healthcheck() (
  kill() { return 0; }
  . "$HEALTHCHECK"
)

cat >"$TEST_ROOT/nc" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$FLOWIE_HEALTH_TEST_CALLS"
if [ -n "${FLOWIE_HEALTH_TEST_FAIL_PORT:-}" ] && [ "$5" = "$FLOWIE_HEALTH_TEST_FAIL_PORT" ]; then
  exit 1
fi
EOF
chmod +x "$TEST_ROOT/nc"

FLOWIE_HEALTH_TEST_CALLS="$TEST_ROOT/primary-calls.txt" \
FLOWIE_HEALTH_HOST=127.0.0.2 \
FLOWIE_HEALTH_PORT=18884 \
PATH="$TEST_ROOT:$PATH" \
  run_healthcheck
printf '%s\n' '-z -w 3 127.0.0.2 18884' >"$TEST_ROOT/primary-expected.txt"
cmp "$TEST_ROOT/primary-expected.txt" "$TEST_ROOT/primary-calls.txt"

FLOWIE_HEALTH_TEST_CALLS="$TEST_ROOT/combined-calls.txt" \
FLOWIE_HEALTH_HOST=127.0.0.3 \
FLOWIE_HEALTH_PORT=18885 \
FLOWIE_HEALTH_SECONDARY_HOST=127.0.0.4 \
FLOWIE_HEALTH_SECONDARY_PORT=8444 \
PATH="$TEST_ROOT:$PATH" \
  run_healthcheck
cat >"$TEST_ROOT/combined-expected.txt" <<'EOF'
-z -w 3 127.0.0.3 18885
-z -w 3 127.0.0.4 8444
EOF
cmp "$TEST_ROOT/combined-expected.txt" "$TEST_ROOT/combined-calls.txt"

if FLOWIE_HEALTH_TEST_CALLS="$TEST_ROOT/failed-calls.txt" \
  FLOWIE_HEALTH_TEST_FAIL_PORT=8445 \
  FLOWIE_HEALTH_PORT=18886 \
  FLOWIE_HEALTH_SECONDARY_PORT=8445 \
  PATH="$TEST_ROOT:$PATH" \
  run_healthcheck; then
  echo "flowie healthcheck accepted an unreachable secondary listener" >&2
  exit 1
fi

if FLOWIE_HEALTH_TEST_CALLS="$TEST_ROOT/invalid-calls.txt" \
  FLOWIE_HEALTH_PORT=18887 \
  FLOWIE_HEALTH_SECONDARY_PORT=invalid \
  PATH="$TEST_ROOT:$PATH" \
  run_healthcheck; then
  echo "flowie healthcheck accepted an invalid secondary port" >&2
  exit 1
fi
test ! -e "$TEST_ROOT/invalid-calls.txt"

echo "flowie healthcheck contract: PASS"
