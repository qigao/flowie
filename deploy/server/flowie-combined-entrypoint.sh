#!/bin/sh
set -eu

if [ "$#" -gt 0 ]; then
  exec "$@"
fi

flowie_combined_validate_bool() {
  case "$2" in
    0|1|false|true|no|yes|off|on) ;;
    *)
      echo "flowie-combined entrypoint: $1 must be one of 0, 1, false, true, no, yes, off, on" >&2
      exit 64
      ;;
  esac
}

: "${FLOWIE_SERVER_ENTRYPOINT:=/usr/local/bin/docker-entrypoint.sh}"
: "${FLOWIE_CONTROL_ENTRYPOINT:=/usr/local/bin/flowie-control-entrypoint}"
: "${FLOWIE_COMBINED_CHECK:=0}"

flowie_combined_validate_bool FLOWIE_COMBINED_CHECK "$FLOWIE_COMBINED_CHECK"
if [ ! -x "$FLOWIE_SERVER_ENTRYPOINT" ]; then
  echo "flowie-combined entrypoint: FLOWIE_SERVER_ENTRYPOINT must name an executable file" >&2
  exit 64
fi
if [ ! -x "$FLOWIE_CONTROL_ENTRYPOINT" ]; then
  echo "flowie-combined entrypoint: FLOWIE_CONTROL_ENTRYPOINT must name an executable file" >&2
  exit 64
fi

case "$FLOWIE_COMBINED_CHECK" in
  1|true|yes|on)
    FLOWIE_CHECK=1 "$FLOWIE_SERVER_ENTRYPOINT"
    FLOWIE_CONTROL_CHECK=1 "$FLOWIE_CONTROL_ENTRYPOINT"
    exit 0
    ;;
esac

server_pid=
control_pid=

flowie_combined_stop() {
  stop_signal=$1
  stop_status=$2
  trap - TERM INT HUP
  set +e
  if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
    kill -"$stop_signal" "$server_pid" 2>/dev/null
  fi
  if [ -n "$control_pid" ] && kill -0 "$control_pid" 2>/dev/null; then
    kill -"$stop_signal" "$control_pid" 2>/dev/null
  fi
  if [ -n "$server_pid" ]; then wait "$server_pid" 2>/dev/null; fi
  if [ -n "$control_pid" ]; then wait "$control_pid" 2>/dev/null; fi
  exit "$stop_status"
}

trap 'flowie_combined_stop TERM 143' TERM
trap 'flowie_combined_stop INT 130' INT
trap 'flowie_combined_stop HUP 129' HUP

"$FLOWIE_SERVER_ENTRYPOINT" &
server_pid=$!
"$FLOWIE_CONTROL_ENTRYPOINT" &
control_pid=$!

while kill -0 "$server_pid" 2>/dev/null && kill -0 "$control_pid" 2>/dev/null; do
  sleep 0.1
done

trap - TERM INT HUP
set +e
if ! kill -0 "$server_pid" 2>/dev/null; then
  wait "$server_pid"
  child_status=$?
  failed_service=flowie_server
  if kill -0 "$control_pid" 2>/dev/null; then kill -TERM "$control_pid" 2>/dev/null; fi
  wait "$control_pid" 2>/dev/null
else
  wait "$control_pid"
  child_status=$?
  failed_service=flowie-control
  if kill -0 "$server_pid" 2>/dev/null; then kill -TERM "$server_pid" 2>/dev/null; fi
  wait "$server_pid" 2>/dev/null
fi

if [ "$child_status" -eq 0 ]; then
  echo "flowie-combined entrypoint: $failed_service exited unexpectedly with status 0" >&2
  child_status=1
fi
exit "$child_status"
