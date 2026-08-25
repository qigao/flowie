#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ENTRYPOINT="$SCRIPT_DIR/../flowie-control-entrypoint.sh"
TEST_ROOT=$(mktemp -d)
trap 'rm -rf "$TEST_ROOT"' EXIT HUP INT TERM

cat >"$TEST_ROOT/flowie-control" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"$FLOWIE_CONTROL_TEST_ARGUMENTS"
if [ -n "${FLOWIE_CONTROL_TEST_SECRET:-}" ]; then
  printf '%s' "${FLOWIE_CONTROL_KEY_PASSWORD:-}" >"$FLOWIE_CONTROL_TEST_SECRET"
fi
EOF
chmod +x "$TEST_ROOT/flowie-control"

CONTROL_CONFIG="$TEST_ROOT/control.yml"
printf '%s\n' 'version: 1' >"$CONTROL_CONFIG"

FLOWIE_CONTROL_TEST_ARGUMENTS="$TEST_ROOT/default-arguments.txt" \
FLOWIE_CONTROL_CONFIG="$CONTROL_CONFIG" \
PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"

cat >"$TEST_ROOT/default-expected.txt" <<EOF
--config
$CONTROL_CONFIG
EOF
cmp "$TEST_ROOT/default-expected.txt" "$TEST_ROOT/default-arguments.txt"

FLOWIE_CONTROL_TEST_ARGUMENTS="$TEST_ROOT/check-arguments.txt" \
FLOWIE_CONTROL_CONFIG="$CONTROL_CONFIG" \
FLOWIE_CONTROL_CHECK=yes \
PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"

cat >"$TEST_ROOT/check-expected.txt" <<EOF
--config
$CONTROL_CONFIG
--check
EOF
cmp "$TEST_ROOT/check-expected.txt" "$TEST_ROOT/check-arguments.txt"

printf '%s' 'container-secret' >"$TEST_ROOT/key-password"
FLOWIE_CONTROL_TEST_ARGUMENTS="$TEST_ROOT/secret-arguments.txt" \
FLOWIE_CONTROL_TEST_SECRET="$TEST_ROOT/imported-secret.txt" \
FLOWIE_CONTROL_CONFIG="$CONTROL_CONFIG" \
FLOWIE_CONTROL_KEY_PASSWORD_FILE="$TEST_ROOT/key-password" \
PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"
test "$(sed -n '1p' "$TEST_ROOT/imported-secret.txt")" = 'container-secret'

if FLOWIE_CONTROL_TEST_ARGUMENTS="$TEST_ROOT/conflict-arguments.txt" \
  FLOWIE_CONTROL_CONFIG="$CONTROL_CONFIG" \
  FLOWIE_CONTROL_KEY_PASSWORD=environment-secret \
  FLOWIE_CONTROL_KEY_PASSWORD_FILE="$TEST_ROOT/key-password" \
  PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"; then
  echo "flowie-control entrypoint accepted two key password sources" >&2
  exit 1
fi
test ! -e "$TEST_ROOT/conflict-arguments.txt"

FLOWIE_CONTROL_TEST_PASSTHROUGH=ok sh "$ENTRYPOINT" sh -c \
  'test "$FLOWIE_CONTROL_TEST_PASSTHROUGH" = ok'

if FLOWIE_CONTROL_TEST_ARGUMENTS="$TEST_ROOT/invalid-arguments.txt" \
  FLOWIE_CONTROL_CONFIG="$CONTROL_CONFIG" \
  FLOWIE_CONTROL_CHECK=maybe \
  PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"; then
  echo "flowie-control entrypoint accepted invalid FLOWIE_CONTROL_CHECK" >&2
  exit 1
fi
test ! -e "$TEST_ROOT/invalid-arguments.txt"

if FLOWIE_CONTROL_TEST_ARGUMENTS="$TEST_ROOT/missing-arguments.txt" \
  FLOWIE_CONTROL_CONFIG="$TEST_ROOT/missing.yml" \
  PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"; then
  echo "flowie-control entrypoint accepted a missing configuration" >&2
  exit 1
fi
test ! -e "$TEST_ROOT/missing-arguments.txt"

echo "flowie-control entrypoint contract: PASS"
