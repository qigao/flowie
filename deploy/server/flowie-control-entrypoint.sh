#!/bin/sh
set -eu

if [ "$#" -gt 0 ]; then
  exec "$@"
fi

: "${FLOWIE_CONTROL_CONFIG:=/etc/flowie/control.yml}"
: "${FLOWIE_CONTROL_CHECK:=0}"

if [ ! -f "$FLOWIE_CONTROL_CONFIG" ] || [ ! -r "$FLOWIE_CONTROL_CONFIG" ]; then
  echo "flowie-control entrypoint: FLOWIE_CONTROL_CONFIG must name a readable file" >&2
  exit 64
fi

if [ -n "${FLOWIE_CONTROL_KEY_PASSWORD_FILE:-}" ]; then
  if [ -n "${FLOWIE_CONTROL_KEY_PASSWORD:-}" ]; then
    echo "flowie-control entrypoint: set only one key password source" >&2
    exit 64
  fi
  if [ ! -f "$FLOWIE_CONTROL_KEY_PASSWORD_FILE" ] ||
     [ ! -r "$FLOWIE_CONTROL_KEY_PASSWORD_FILE" ]; then
    echo "flowie-control entrypoint: FLOWIE_CONTROL_KEY_PASSWORD_FILE must name a readable file" >&2
    exit 64
  fi
  FLOWIE_CONTROL_KEY_PASSWORD=$(cat "$FLOWIE_CONTROL_KEY_PASSWORD_FILE")
  export FLOWIE_CONTROL_KEY_PASSWORD
fi

set -- flowie-control --config "$FLOWIE_CONTROL_CONFIG"
case "$FLOWIE_CONTROL_CHECK" in
  1|true|yes|on) set -- "$@" --check ;;
  0|false|no|off) ;;
  *)
    echo "flowie-control entrypoint: FLOWIE_CONTROL_CHECK must be a boolean" >&2
    exit 64
    ;;
esac

exec "$@"
