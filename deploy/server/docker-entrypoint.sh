#!/bin/sh
set -eu

if [ "$#" -gt 0 ]; then
  exec "$@"
fi

flowie_validate_bool() {
  case "$2" in
    0|1|false|true|no|yes|off|on) ;;
    *)
      echo "flowie entrypoint: $1 must be one of 0, 1, false, true, no, yes, off, on" >&2
      exit 64
      ;;
  esac
}

: "${FLOWIE_CONFIG:=/etc/flowie/flowie.yml}"
: "${FLOWIE_PROFILE:=flowie}"
: "${FLOWIE_PROTOCOL_STORE_DRIVER:=sqlite}"
if [ "${FLOWIE_PROTOCOL_STORE_OPTIONS+x}" != x ]; then
  FLOWIE_PROTOCOL_STORE_OPTIONS='{"filename":"/var/lib/flowie/flowie-protocol.sqlite3"}'
fi
: "${FLOWIE_CHECK:=0}"

if [ -n "${FLOWIE_AUTH_SERVICE_TOKEN_FILE:-}" ]; then
  if [ -n "${FLOWIE_AUTH_SERVICE_TOKEN:-}" ]; then
    echo "flowie entrypoint: set only one auth service token source" >&2
    exit 64
  fi
  if [ ! -f "$FLOWIE_AUTH_SERVICE_TOKEN_FILE" ] ||
     [ ! -r "$FLOWIE_AUTH_SERVICE_TOKEN_FILE" ]; then
    echo "flowie entrypoint: FLOWIE_AUTH_SERVICE_TOKEN_FILE must name a readable file" >&2
    exit 64
  fi
  FLOWIE_AUTH_SERVICE_TOKEN=$(cat "$FLOWIE_AUTH_SERVICE_TOKEN_FILE")
  if [ -z "$FLOWIE_AUTH_SERVICE_TOKEN" ]; then
    echo "flowie entrypoint: auth service token must not be empty" >&2
    exit 64
  fi
  export FLOWIE_AUTH_SERVICE_TOKEN
fi

flowie_validate_bool FLOWIE_CHECK "$FLOWIE_CHECK"
if [ ! -r "$FLOWIE_CONFIG" ]; then
  echo "flowie entrypoint: required file is not readable: $FLOWIE_CONFIG" >&2
  exit 66
fi

set -- flowie_server --require-security --config "$FLOWIE_CONFIG" --profile "$FLOWIE_PROFILE"
case "$FLOWIE_CHECK" in
  1|true|yes|on) set -- "$@" --check ;;
esac
set -- "$@" \
  --protocol-store-driver "$FLOWIE_PROTOCOL_STORE_DRIVER" \
  --protocol-store-options "$FLOWIE_PROTOCOL_STORE_OPTIONS"
exec "$@"
