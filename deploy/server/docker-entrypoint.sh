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

: "${FLOWIE_HOST:=127.0.0.1}"
: "${FLOWIE_PORT:=18883}"
: "${FLOWIE_TRANSPORT:=tcp}"
: "${FLOWIE_PATH:=/mqtt}"
: "${FLOWIE_PROTOCOL_STORE_DRIVER:=sqlite}"
if [ "${FLOWIE_PROTOCOL_STORE_OPTIONS+x}" != x ]; then
  FLOWIE_PROTOCOL_STORE_OPTIONS='{"filename":"/var/lib/flowie/flowie-protocol.sqlite3"}'
fi
export FLOWIE_PROTOCOL_STORE_DRIVER FLOWIE_PROTOCOL_STORE_OPTIONS
: "${FLOWIE_MAX_PACKET_SIZE:=1048576}"
: "${FLOWIE_MAX_CONNECTIONS:=1024}"
: "${FLOWIE_MAX_SESSIONS:=$FLOWIE_MAX_CONNECTIONS}"
: "${FLOWIE_MAX_SUBSCRIPTIONS_PER_SESSION:=1024}"
: "${FLOWIE_MAX_INFLIGHT_PER_SESSION:=64}"
: "${FLOWIE_MAX_RETAINED_MESSAGES:=$FLOWIE_MAX_SESSIONS}"
: "${FLOWIE_SEND_HWM_BYTES:=1048576}"
: "${FLOWIE_COROUTINE_STACK_SIZE:=0}"
: "${FLOWIE_STREAM_RECV_BUFFER_BYTES:=0}"
: "${FLOWIE_SOCKET_RECV_BUFFER_BYTES:=0}"
: "${FLOWIE_SOCKET_SEND_BUFFER_BYTES:=0}"
: "${FLOWIE_TIMEOUT_MS:=0}"
: "${FLOWIE_RECV_TIMEOUT_MS:=0}"
: "${FLOWIE_TCP_KEEPALIVE_IDLE_MS:=0}"
: "${FLOWIE_TCP_KEEPALIVE_INTERVAL_MS:=0}"
: "${FLOWIE_TCP_KEEPALIVE_COUNT:=0}"
: "${FLOWIE_LOG_LEVEL:=INFO}"
: "${FLOWIE_TCP_KEEPALIVE:=0}"
: "${FLOWIE_REUSE_PORT:=0}"
: "${FLOWIE_CHECK:=0}"

flowie_validate_bool FLOWIE_TCP_KEEPALIVE "$FLOWIE_TCP_KEEPALIVE"
flowie_validate_bool FLOWIE_REUSE_PORT "$FLOWIE_REUSE_PORT"
flowie_validate_bool FLOWIE_CHECK "$FLOWIE_CHECK"

set -- flowie_server
case "$FLOWIE_CHECK" in
  1|true|yes|on) set -- "$@" --check ;;
esac
set -- "$@" \
  --host "$FLOWIE_HOST" \
  --port "$FLOWIE_PORT" \
  --transport "$FLOWIE_TRANSPORT" \
  --path "$FLOWIE_PATH" \
  --max-packet-size "$FLOWIE_MAX_PACKET_SIZE" \
  --max-connections "$FLOWIE_MAX_CONNECTIONS" \
  --max-sessions "$FLOWIE_MAX_SESSIONS" \
  --max-subscriptions-per-session "$FLOWIE_MAX_SUBSCRIPTIONS_PER_SESSION" \
  --max-inflight "$FLOWIE_MAX_INFLIGHT_PER_SESSION" \
  --max-retained-messages "$FLOWIE_MAX_RETAINED_MESSAGES" \
  --send-hwm-bytes "$FLOWIE_SEND_HWM_BYTES" \
  --coroutine-stack-size "$FLOWIE_COROUTINE_STACK_SIZE" \
  --stream-recv-buffer-bytes "$FLOWIE_STREAM_RECV_BUFFER_BYTES" \
  --socket-recv-buffer-bytes "$FLOWIE_SOCKET_RECV_BUFFER_BYTES" \
  --socket-send-buffer-bytes "$FLOWIE_SOCKET_SEND_BUFFER_BYTES" \
  --timeout-ms "$FLOWIE_TIMEOUT_MS" \
  --recv-timeout-ms "$FLOWIE_RECV_TIMEOUT_MS"
case "$FLOWIE_TCP_KEEPALIVE" in
  1|true|yes|on) set -- "$@" --tcp-keepalive ;;
esac
set -- "$@" \
  --tcp-keepalive-idle-ms "$FLOWIE_TCP_KEEPALIVE_IDLE_MS" \
  --tcp-keepalive-interval-ms "$FLOWIE_TCP_KEEPALIVE_INTERVAL_MS" \
  --tcp-keepalive-count "$FLOWIE_TCP_KEEPALIVE_COUNT"
case "$FLOWIE_REUSE_PORT" in
  1|true|yes|on) set -- "$@" --reuse-port ;;
esac
set -- "$@" --log-level "$FLOWIE_LOG_LEVEL"
exec "$@"
