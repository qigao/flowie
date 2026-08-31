#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ENTRYPOINT="$SCRIPT_DIR/../docker-entrypoint.sh"
TEST_ROOT=$(mktemp -d)
trap 'rm -rf "$TEST_ROOT"' EXIT HUP INT TERM

cat >"$TEST_ROOT/flowie_server" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"$FLOWIE_TEST_ARGUMENTS"
printf '%s\n%s\n%s\n' "$FLOWIE_PROTOCOL_STORE_DRIVER" "$FLOWIE_PROTOCOL_STORE_OPTIONS" \
  "$FLOWIE_AUTH_SERVICE_TOKEN" \
  >"$FLOWIE_TEST_STORAGE"
EOF
chmod +x "$TEST_ROOT/flowie_server"
printf '%s\n' 'version: 1' >"$TEST_ROOT/flowie.yml"
printf '%s\n' 'test-service-token' >"$TEST_ROOT/service-token"

FLOWIE_TEST_ARGUMENTS="$TEST_ROOT/arguments.txt" \
FLOWIE_TEST_STORAGE="$TEST_ROOT/storage.txt" \
FLOWIE_CONFIG="$TEST_ROOT/flowie.yml" \
FLOWIE_PROFILE=eu \
FLOWIE_AUTH_SERVICE_TOKEN_FILE="$TEST_ROOT/service-token" \
FLOWIE_PROTOCOL_STORE_DRIVER=postgresql \
FLOWIE_PROTOCOL_STORE_OPTIONS='{"conninfo":"host=postgres dbname=flowie"}' \
PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"

cat >"$TEST_ROOT/expected.txt" <<EOF
--require-security
--config
$TEST_ROOT/flowie.yml
--profile
eu
--protocol-store-driver
postgresql
--protocol-store-options
{"conninfo":"host=postgres dbname=flowie"}
EOF

cmp "$TEST_ROOT/expected.txt" "$TEST_ROOT/arguments.txt"
cat >"$TEST_ROOT/expected-storage.txt" <<'EOF'
postgresql
{"conninfo":"host=postgres dbname=flowie"}
test-service-token
EOF
cmp "$TEST_ROOT/expected-storage.txt" "$TEST_ROOT/storage.txt"

FLOWIE_TEST_ARGUMENTS="$TEST_ROOT/check-arguments.txt" \
FLOWIE_TEST_STORAGE="$TEST_ROOT/check-storage.txt" \
FLOWIE_CONFIG="$TEST_ROOT/flowie.yml" \
FLOWIE_AUTH_SERVICE_TOKEN_FILE="$TEST_ROOT/service-token" \
FLOWIE_CHECK=yes \
PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"
grep -qx -- '--check' "$TEST_ROOT/check-arguments.txt"

FLOWIE_TEST_PASSTHROUGH=ok sh "$ENTRYPOINT" sh -c \
  'test "$FLOWIE_TEST_PASSTHROUGH" = ok'

if FLOWIE_TEST_ARGUMENTS="$TEST_ROOT/missing-arguments.txt" \
  FLOWIE_TEST_STORAGE="$TEST_ROOT/missing-storage.txt" \
  FLOWIE_CONFIG="$TEST_ROOT/missing.yml" \
  PATH="$TEST_ROOT:$PATH" sh "$ENTRYPOINT"; then
  echo "docker-entrypoint accepted an unreadable Flowie configuration" >&2
  exit 1
fi
test ! -e "$TEST_ROOT/missing-arguments.txt"

if FLOWIE_TEST_ARGUMENTS="$TEST_ROOT/invalid-check-arguments.txt" \
  FLOWIE_TEST_STORAGE="$TEST_ROOT/invalid-check-storage.txt" \
  FLOWIE_CONFIG="$TEST_ROOT/flowie.yml" \
  FLOWIE_AUTH_SERVICE_TOKEN_FILE="$TEST_ROOT/service-token" \
  FLOWIE_CHECK=maybe \
  PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"; then
  echo "docker-entrypoint accepted invalid FLOWIE_CHECK" >&2
  exit 1
fi
test ! -e "$TEST_ROOT/invalid-check-arguments.txt"

echo "docker-entrypoint contract: PASS"
