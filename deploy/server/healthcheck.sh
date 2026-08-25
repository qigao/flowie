#!/bin/sh
set -eu

: "${FLOWIE_HEALTH_HOST:=127.0.0.1}"
: "${FLOWIE_HEALTH_PORT:=18883}"
: "${FLOWIE_HEALTH_SECONDARY_HOST:=127.0.0.1}"
: "${FLOWIE_HEALTH_SECONDARY_PORT:=}"

flowie_health_validate_port() {
  case "$2" in
    ''|*[!0-9]*)
      echo "flowie healthcheck: $1 must be numeric" >&2
      exit 2
      ;;
  esac

  if [ "$2" -lt 1 ] || [ "$2" -gt 65535 ]; then
    echo "flowie healthcheck: $1 is outside 1..65535" >&2
    exit 2
  fi
}

flowie_health_validate_port FLOWIE_HEALTH_PORT "$FLOWIE_HEALTH_PORT"
if [ -n "$FLOWIE_HEALTH_SECONDARY_PORT" ]; then
  flowie_health_validate_port FLOWIE_HEALTH_SECONDARY_PORT "$FLOWIE_HEALTH_SECONDARY_PORT"
fi

kill -0 1
nc -z -w 3 "$FLOWIE_HEALTH_HOST" "$FLOWIE_HEALTH_PORT"
if [ -n "$FLOWIE_HEALTH_SECONDARY_PORT" ]; then
  nc -z -w 3 "$FLOWIE_HEALTH_SECONDARY_HOST" "$FLOWIE_HEALTH_SECONDARY_PORT"
fi
