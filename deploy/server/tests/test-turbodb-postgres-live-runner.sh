#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNNER="$SCRIPT_DIR/run-turbodb-postgres-live.sh"
TEST_ROOT=$(mktemp -d)
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

FAKE_BIN="$TEST_ROOT/bin"
BUILD_DIR="$TEST_ROOT/build"
DOCKER_LOG="$TEST_ROOT/docker.log"
CTEST_LOG="$TEST_ROOT/ctest.log"
mkdir -p -- "$FAKE_BIN" "$BUILD_DIR"
: > "$DOCKER_LOG"
: > "$CTEST_LOG"

cat > "$FAKE_BIN/docker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$FLOWIE_TEST_DOCKER_LOG"
case "${1:-}" in
  run)
    printf '%s\n' flowie-postgres-container-id
    ;;
  inspect)
    printf '%s\n' healthy
    ;;
  port)
    printf '%s\n' '127.0.0.1:35432'
    ;;
  logs)
    printf '%s\n' 'database system is ready to accept connections'
    ;;
  rm)
    ;;
  *)
    printf 'unexpected docker command: %s\n' "${1:-}" >&2
    exit 64
    ;;
esac
EOF

cat > "$FAKE_BIN/ctest" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'FLOWIE_TURBODB_TEST_CONNINFO=%s\n' "${FLOWIE_TURBODB_TEST_CONNINFO:-}" \
  >> "$FLOWIE_TEST_CTEST_LOG"
printf '%s\n' "$*" >> "$FLOWIE_TEST_CTEST_LOG"
previous=
for argument in "$@"; do
  if [[ "$previous" == '--output-junit' ]]; then
    : > "$argument"
  fi
  previous=$argument
done
exit "${FLOWIE_TEST_CTEST_STATUS:-0}"
EOF

cat > "$FAKE_BIN/openssl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[[ "${1:-}" == rand && "${2:-}" == -hex && "${3:-}" == 24 ]]
printf '%s\n' '0123456789abcdef0123456789abcdef0123456789abcdef'
EOF

chmod +x "$FAKE_BIN/docker" "$FAKE_BIN/ctest" "$FAKE_BIN/openssl"

export FLOWIE_TEST_DOCKER_LOG="$DOCKER_LOG"
export FLOWIE_TEST_CTEST_LOG="$CTEST_LOG"
export PATH="$FAKE_BIN:$PATH"

SUCCESS_ARTIFACTS="$TEST_ROOT/success-artifacts"
bash "$RUNNER" \
  --build-dir "$BUILD_DIR" \
  --artifacts "$SUCCESS_ARTIFACTS" \
  --image postgres:test \
  --health-attempts 2

grep -Fq -- '--publish 127.0.0.1::5432' "$DOCKER_LOG"
grep -Fq -- '--tmpfs /var/lib/postgresql/data:rw,noexec,nosuid,size=256m' "$DOCKER_LOG"
grep -Fq -- '--env POSTGRES_PASSWORD_FILE=/run/secrets/postgres-password' "$DOCKER_LOG"
grep -Fq -- 'postgres:test' "$DOCKER_LOG"
if grep -F '0123456789abcdef0123456789abcdef0123456789abcdef' "$DOCKER_LOG"; then
  printf 'runner exposed the PostgreSQL password in Docker arguments\n' >&2
  exit 1
fi
grep -Fq \
  'FLOWIE_TURBODB_TEST_CONNINFO=host=127.0.0.1 port=35432 dbname=flowie_test user=flowie_test password=0123456789abcdef0123456789abcdef0123456789abcdef sslmode=disable' \
  "$CTEST_LOG"
grep -Fq -- '--no-tests=error' "$CTEST_LOG"
grep -Fq -- 'test_flowie_protocol_repository_turbodb_live|test_flowie_control_turbodb_live' \
  "$CTEST_LOG"
test -f "$SUCCESS_ARTIFACTS/turbodb-postgres-live.xml"
test -f "$SUCCESS_ARTIFACTS/postgres.log"
grep -Fq -- 'rm -f' "$DOCKER_LOG"

FAILURE_ARTIFACTS="$TEST_ROOT/failure-artifacts"
set +e
FLOWIE_TEST_CTEST_STATUS=7 bash "$RUNNER" \
  --build-dir "$BUILD_DIR" \
  --artifacts "$FAILURE_ARTIFACTS" \
  --image postgres:test \
  --health-attempts 2
STATUS=$?
set -e
if ((STATUS != 7)); then
  printf 'runner did not preserve the CTest failure status: %d\n' "$STATUS" >&2
  exit 1
fi
test -f "$FAILURE_ARTIFACTS/postgres.log"
test "$(grep -c '^rm -f ' "$DOCKER_LOG")" = 2

printf 'TurboDB PostgreSQL live runner contract: PASS\n'
