#!/usr/bin/env bash

set -euo pipefail

POSTGRES_IMAGE=postgres:17.6-alpine3.22
POSTGRES_DATABASE=flowie_test
POSTGRES_USER=flowie_test
HEALTH_ATTEMPTS=30
HEALTH_INTERVAL_SECONDS=2
BUILD_DIR=
ARTIFACT_ROOT=
CONTAINER_NAME=
CONTAINER_STARTED=0
PASSWORD_FILE=
CLEANUP_ATTEMPTED=0

usage() {
  cat <<'EOF'
Usage: run-turbodb-postgres-live.sh --build-dir DIR --artifacts DIR [options]

Required:
  --build-dir DIR             Configured Flowie build with TurboDB live tests enabled
  --artifacts DIR             New directory for JUnit output and PostgreSQL logs

Options:
  --image IMAGE               PostgreSQL image (default: postgres:17.6-alpine3.22)
  --health-attempts N         Maximum health inspections (default: 30)
  --health-interval-seconds N Seconds between health inspections (default: 2)
  --help                      Show this help
EOF
}

fail() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

cleanup_resources() {
  local cleanup_status=0

  CLEANUP_ATTEMPTED=1
  if ((CONTAINER_STARTED)); then
    if ! docker logs "$CONTAINER_NAME" > "$ARTIFACT_ROOT/postgres.log" 2>&1; then
      printf 'ERROR: failed to collect PostgreSQL logs: %s\n' "$CONTAINER_NAME" >&2
      cleanup_status=1
    fi
    if docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1; then
      CONTAINER_STARTED=0
    else
      printf 'ERROR: failed to remove PostgreSQL container: %s\n' "$CONTAINER_NAME" >&2
      cleanup_status=1
    fi
  fi
  if [[ -n "$PASSWORD_FILE" ]]; then
    if rm -f -- "$PASSWORD_FILE"; then
      PASSWORD_FILE=
    else
      printf 'ERROR: failed to remove PostgreSQL password file\n' >&2
      cleanup_status=1
    fi
  fi
  return "$cleanup_status"
}

cleanup_on_exit() {
  local status=$?
  local cleanup_status=0

  trap - EXIT
  if ((!CLEANUP_ATTEMPTED)); then
    cleanup_resources || cleanup_status=$?
  fi
  if ((status != 0)); then
    exit "$status"
  fi
  exit "$cleanup_status"
}

trap cleanup_on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

while (($# > 0)); do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || fail '--build-dir requires a value'
      BUILD_DIR=$2
      shift 2
      ;;
    --artifacts)
      [[ $# -ge 2 ]] || fail '--artifacts requires a value'
      ARTIFACT_ROOT=$2
      shift 2
      ;;
    --image)
      [[ $# -ge 2 ]] || fail '--image requires a value'
      POSTGRES_IMAGE=$2
      shift 2
      ;;
    --health-attempts)
      [[ $# -ge 2 ]] || fail '--health-attempts requires a value'
      HEALTH_ATTEMPTS=$2
      shift 2
      ;;
    --health-interval-seconds)
      [[ $# -ge 2 ]] || fail '--health-interval-seconds requires a value'
      HEALTH_INTERVAL_SECONDS=$2
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      fail "unknown argument: $1"
      ;;
  esac
done

[[ -n "$BUILD_DIR" ]] || fail '--build-dir is required'
[[ -d "$BUILD_DIR" ]] || fail "build directory does not exist: $BUILD_DIR"
[[ -n "$ARTIFACT_ROOT" ]] || fail '--artifacts is required'
[[ ! -e "$ARTIFACT_ROOT" ]] || fail "artifact path already exists: $ARTIFACT_ROOT"
[[ -n "$POSTGRES_IMAGE" ]] || fail '--image must not be empty'
is_positive_integer "$HEALTH_ATTEMPTS" || fail '--health-attempts must be a positive integer'
is_positive_integer "$HEALTH_INTERVAL_SECONDS" ||
  fail '--health-interval-seconds must be a positive integer'

for command in ctest docker openssl; do
  command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

mkdir -p -- "$ARTIFACT_ROOT"
PASSWORD_FILE=$(mktemp)
chmod 0600 "$PASSWORD_FILE"
POSTGRES_PASSWORD=$(openssl rand -hex 24)
[[ "$POSTGRES_PASSWORD" =~ ^[0-9a-f]{48}$ ]] || fail 'openssl returned an invalid password'
printf '%s\n' "$POSTGRES_PASSWORD" > "$PASSWORD_FILE"

CONTAINER_NAME="flowie-turbodb-postgres-$(date -u +%Y%m%dT%H%M%SZ)-$$"
docker run --detach \
  --name "$CONTAINER_NAME" \
  --publish 127.0.0.1::5432 \
  --tmpfs /var/lib/postgresql/data:rw,noexec,nosuid,size=256m \
  --mount "type=bind,src=$PASSWORD_FILE,dst=/run/secrets/postgres-password,readonly" \
  --env POSTGRES_PASSWORD_FILE=/run/secrets/postgres-password \
  --env "POSTGRES_DB=$POSTGRES_DATABASE" \
  --env "POSTGRES_USER=$POSTGRES_USER" \
  --health-cmd "pg_isready --username=$POSTGRES_USER --dbname=$POSTGRES_DATABASE" \
  --health-interval 2s \
  --health-timeout 5s \
  --health-retries 15 \
  "$POSTGRES_IMAGE" >/dev/null
CONTAINER_STARTED=1

POSTGRES_HEALTH=
for ((attempt = 1; attempt <= HEALTH_ATTEMPTS; ++attempt)); do
  POSTGRES_HEALTH=$(docker inspect --format '{{.State.Health.Status}}' "$CONTAINER_NAME")
  case "$POSTGRES_HEALTH" in
    healthy)
      break
      ;;
    starting)
      ;;
    unhealthy)
      fail "PostgreSQL container became unhealthy: $CONTAINER_NAME"
      ;;
    *)
      fail "unexpected PostgreSQL health status: $POSTGRES_HEALTH"
      ;;
  esac
  if ((attempt == HEALTH_ATTEMPTS)); then
    fail "PostgreSQL health timeout after $HEALTH_ATTEMPTS attempts"
  fi
  sleep "$HEALTH_INTERVAL_SECONDS"
done

POSTGRES_BINDING=$(docker port "$CONTAINER_NAME" 5432/tcp)
POSTGRES_PORT=${POSTGRES_BINDING##*:}
is_positive_integer "$POSTGRES_PORT" || fail "invalid PostgreSQL port binding: $POSTGRES_BINDING"
((POSTGRES_PORT <= 65535)) || fail "PostgreSQL port exceeds 65535: $POSTGRES_PORT"

cat > "$ARTIFACT_ROOT/postgres-metadata.txt" <<EOF
image=$POSTGRES_IMAGE
container=$CONTAINER_NAME
host=127.0.0.1
port=$POSTGRES_PORT
database=$POSTGRES_DATABASE
user=$POSTGRES_USER
data=tmpfs
EOF

export FLOWIE_TURBODB_TEST_CONNINFO="host=127.0.0.1 port=$POSTGRES_PORT dbname=$POSTGRES_DATABASE user=$POSTGRES_USER password=$POSTGRES_PASSWORD sslmode=disable"

ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error \
  -R '^(test_flowie_protocol_repository_turbodb_live|test_flowie_control_turbodb_live)$' \
  --output-junit "$ARTIFACT_ROOT/turbodb-postgres-live.xml"

cleanup_resources || fail 'PostgreSQL test resources could not be cleaned up'
trap - EXIT
printf 'TurboDB PostgreSQL live gate: PASS artifacts=%s\n' "$ARTIFACT_ROOT"
