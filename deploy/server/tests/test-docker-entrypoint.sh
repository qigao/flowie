#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ENTRYPOINT="$SCRIPT_DIR/../docker-entrypoint.sh"
TEST_ROOT=$(mktemp -d)
trap 'rm -rf "$TEST_ROOT"' EXIT HUP INT TERM

cat >"$TEST_ROOT/flowie_server" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"$FLOWIE_TEST_ARGUMENTS"
printf '%s\n%s\n' "$FLOWIE_PROTOCOL_STORE_DRIVER" "$FLOWIE_PROTOCOL_STORE_OPTIONS" \
  >"$FLOWIE_TEST_STORAGE"
EOF
chmod +x "$TEST_ROOT/flowie_server"

FLOWIE_TEST_ARGUMENTS="$TEST_ROOT/arguments.txt" \
FLOWIE_TEST_STORAGE="$TEST_ROOT/storage.txt" \
FLOWIE_HOST=127.0.0.2 \
FLOWIE_PORT=28883 \
FLOWIE_TRANSPORT=tcp \
FLOWIE_PATH=/mqtt-test \
FLOWIE_PROTOCOL_STORE_DRIVER=postgresql \
FLOWIE_PROTOCOL_STORE_OPTIONS='{"conninfo":"host=postgres dbname=flowie"}' \
FLOWIE_MAX_PACKET_SIZE=262144 \
FLOWIE_MAX_CONNECTIONS=321 \
FLOWIE_MAX_SESSIONS=654 \
FLOWIE_MAX_SUBSCRIPTIONS_PER_SESSION=33 \
FLOWIE_MAX_INFLIGHT_PER_SESSION=17 \
FLOWIE_MAX_RETAINED_MESSAGES=444 \
FLOWIE_SEND_HWM_BYTES=131072 \
FLOWIE_COROUTINE_STACK_SIZE=65536 \
FLOWIE_STREAM_RECV_BUFFER_BYTES=8192 \
FLOWIE_SOCKET_RECV_BUFFER_BYTES=32768 \
FLOWIE_SOCKET_SEND_BUFFER_BYTES=65536 \
FLOWIE_TIMEOUT_MS=30000 \
FLOWIE_RECV_TIMEOUT_MS=15000 \
FLOWIE_TCP_KEEPALIVE=yes \
FLOWIE_TCP_KEEPALIVE_IDLE_MS=60000 \
FLOWIE_TCP_KEEPALIVE_INTERVAL_MS=10000 \
FLOWIE_TCP_KEEPALIVE_COUNT=3 \
FLOWIE_REUSE_PORT=on \
FLOWIE_LOG_LEVEL=DEBUG \
PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"

cat >"$TEST_ROOT/expected.txt" <<'EOF'
--host
127.0.0.2
--port
28883
--transport
tcp
--path
/mqtt-test
--max-packet-size
262144
--max-connections
321
--max-sessions
654
--max-subscriptions-per-session
33
--max-inflight
17
--max-retained-messages
444
--send-hwm-bytes
131072
--coroutine-stack-size
65536
--stream-recv-buffer-bytes
8192
--socket-recv-buffer-bytes
32768
--socket-send-buffer-bytes
65536
--timeout-ms
30000
--recv-timeout-ms
15000
--tcp-keepalive
--tcp-keepalive-idle-ms
60000
--tcp-keepalive-interval-ms
10000
--tcp-keepalive-count
3
--reuse-port
--log-level
DEBUG
EOF

cmp "$TEST_ROOT/expected.txt" "$TEST_ROOT/arguments.txt"
cat >"$TEST_ROOT/expected-storage.txt" <<'EOF'
postgresql
{"conninfo":"host=postgres dbname=flowie"}
EOF
cmp "$TEST_ROOT/expected-storage.txt" "$TEST_ROOT/storage.txt"

FLOWIE_TEST_ARGUMENTS="$TEST_ROOT/check-arguments.txt" \
FLOWIE_TEST_STORAGE="$TEST_ROOT/check-storage.txt" \
FLOWIE_CHECK=yes \
PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"
test "$(sed -n '1p' "$TEST_ROOT/check-arguments.txt")" = '--check'
test "$(sed -n '2p' "$TEST_ROOT/check-arguments.txt")" = '--host'

FLOWIE_TEST_PASSTHROUGH=ok sh "$ENTRYPOINT" sh -c \
  'test "$FLOWIE_TEST_PASSTHROUGH" = ok'

if FLOWIE_TEST_ARGUMENTS="$TEST_ROOT/invalid-arguments.txt" \
  FLOWIE_TEST_STORAGE="$TEST_ROOT/invalid-storage.txt" \
  FLOWIE_TCP_KEEPALIVE=maybe \
  PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"; then
  echo "docker-entrypoint accepted invalid FLOWIE_TCP_KEEPALIVE" >&2
  exit 1
fi

if FLOWIE_TEST_ARGUMENTS="$TEST_ROOT/invalid-check-arguments.txt" \
  FLOWIE_TEST_STORAGE="$TEST_ROOT/invalid-check-storage.txt" \
  FLOWIE_CHECK=maybe \
  PATH="$TEST_ROOT:$PATH" \
  sh "$ENTRYPOINT"; then
  echo "docker-entrypoint accepted invalid FLOWIE_CHECK" >&2
  exit 1
fi
test ! -e "$TEST_ROOT/invalid-check-arguments.txt"

echo "docker-entrypoint contract: PASS"
