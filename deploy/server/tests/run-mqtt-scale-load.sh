#!/usr/bin/env bash

set -uo pipefail

HOST=127.0.0.1
PORT=18890
SERVER_PID=
SERVER_LOG=
ARTIFACT_ROOT=
MAX_CONNECTIONS=
MAX_INFLIGHT=
TIERS=16,32,64
MESSAGES=25
PAYLOAD_BYTES=
TIMEOUT_SECONDS=300
CLIENT_RECEIVE_MAXIMUM=20
MIN_PAYLOAD_BYTES=100
MAX_PAYLOAD_BYTES=4096
MAX_SIGNED_64=9223372036854775807
PLAN_ONLY=0
CHILD_PIDS=()
MONITOR_STOP=
PUBLISHER_GATE=

usage() {
  cat <<'EOF'
Usage: run-mqtt-scale-load.sh [options]

Required for execution:
  --server-pid PID          Native flowie_server PID on this Linux host
  --server-log PATH         Combined stdout/stderr tlog file attached to PID
  --artifacts DIR           New directory for all load evidence
  --max-connections N       Value used to start the server
  --max-inflight N          Per-session value used to start the server

Options:
  --host HOST               MQTT listener host (default: 127.0.0.1)
  --port PORT               MQTT listener port (default: 18890)
  --tiers CSV               Subscribers and publishers per QoS (default: 16,32,64)
  --messages N              Messages per publisher (default: 25)
  --payload-bytes N         Exact ASCII payload bytes, 100..4096 (default: metadata only)
  --timeout N               Per-client timeout in seconds (default: 300)
  --plan                    Validate and print capacity calculations only
  --help                    Show this help
EOF
}

fail() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

checked_product() {
  local label=$1
  shift
  local product=1
  local factor
  for factor in "$@"; do
    if ((factor != 0 && product > MAX_SIGNED_64 / factor)); then
      fail "$label exceeds signed 64-bit capacity"
    fi
    product=$((product * factor))
  done
  printf '%d' "$product"
}

build_payload() {
  local run_id=$1
  local qos=$2
  local publisher=$3
  local sequence=$4
  local payload padding_count padding
  printf -v payload 'run=%s qos=%d publisher=%d sequence=%d marker=flowie-scale-v1' \
    "$run_id" "$qos" "$publisher" "$sequence"
  if [[ -n "$PAYLOAD_BYTES" ]]; then
    if ((${#payload} > PAYLOAD_BYTES)); then
      fail "payload-bytes=$PAYLOAD_BYTES is shorter than metadata bytes=${#payload}"
    fi
    padding_count=$((PAYLOAD_BYTES - ${#payload}))
    printf -v padding '%*s' "$padding_count" ''
    padding=${padding// /x}
    payload+=$padding
    ((${#payload} == PAYLOAD_BYTES)) || fail 'generated payload length mismatch'
  fi
  printf '%s' "$payload"
}

cleanup_children() {
  local pid
  if [[ -n "${PUBLISHER_GATE:-}" ]]; then
    touch "$PUBLISHER_GATE"
  fi
  if [[ -n "${MONITOR_STOP:-}" ]]; then
    rm -f -- "$MONITOR_STOP"
  fi
  for pid in "${CHILD_PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
    fi
  done
  for pid in "${CHILD_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
  CHILD_PIDS=()
}

trap cleanup_children EXIT
trap 'cleanup_children; exit 130' INT
trap 'cleanup_children; exit 143' TERM

while (($# > 0)); do
  case "$1" in
    --host) [[ $# -ge 2 ]] || fail '--host requires a value'; HOST=$2; shift 2 ;;
    --port) [[ $# -ge 2 ]] || fail '--port requires a value'; PORT=$2; shift 2 ;;
    --server-pid) [[ $# -ge 2 ]] || fail '--server-pid requires a value'; SERVER_PID=$2; shift 2 ;;
    --server-log) [[ $# -ge 2 ]] || fail '--server-log requires a value'; SERVER_LOG=$2; shift 2 ;;
    --artifacts) [[ $# -ge 2 ]] || fail '--artifacts requires a value'; ARTIFACT_ROOT=$2; shift 2 ;;
    --max-connections) [[ $# -ge 2 ]] || fail '--max-connections requires a value'; MAX_CONNECTIONS=$2; shift 2 ;;
    --max-inflight) [[ $# -ge 2 ]] || fail '--max-inflight requires a value'; MAX_INFLIGHT=$2; shift 2 ;;
    --tiers) [[ $# -ge 2 ]] || fail '--tiers requires a value'; TIERS=$2; shift 2 ;;
    --messages) [[ $# -ge 2 ]] || fail '--messages requires a value'; MESSAGES=$2; shift 2 ;;
    --payload-bytes) [[ $# -ge 2 ]] || fail '--payload-bytes requires a value'; PAYLOAD_BYTES=$2; shift 2 ;;
    --timeout) [[ $# -ge 2 ]] || fail '--timeout requires a value'; TIMEOUT_SECONDS=$2; shift 2 ;;
    --plan) PLAN_ONLY=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) fail "unknown argument: $1" ;;
  esac
done

is_positive_integer "$PORT" || fail 'port must be a positive integer'
((PORT <= 65535)) || fail 'port must be <= 65535'
is_positive_integer "$MESSAGES" || fail 'messages must be a positive integer'
is_positive_integer "$TIMEOUT_SECONDS" || fail 'timeout must be a positive integer'
if [[ -n "$PAYLOAD_BYTES" ]]; then
  is_positive_integer "$PAYLOAD_BYTES" || fail 'payload-bytes must be a positive integer'
  ((PAYLOAD_BYTES >= MIN_PAYLOAD_BYTES && PAYLOAD_BYTES <= MAX_PAYLOAD_BYTES)) ||
    fail "payload-bytes must be between $MIN_PAYLOAD_BYTES and $MAX_PAYLOAD_BYTES"
fi
IFS=',' read -r -a TIER_VALUES <<< "$TIERS"
((${#TIER_VALUES[@]} > 0)) || fail 'tiers must not be empty'

MAX_TIER=0
for tier in "${TIER_VALUES[@]}"; do
  is_positive_integer "$tier" || fail "invalid tier: $tier"
  ((tier > MAX_TIER)) && MAX_TIER=$tier
done

REQUIRED_CONNECTIONS=$((6 * MAX_TIER))
REQUIRED_INFLIGHT=$((MAX_TIER * MESSAGES))
if [[ -n "$PAYLOAD_BYTES" ]]; then
  MAX_EXPECTED_DELIVERIES=$(checked_product expected-deliveries 3 "$MAX_TIER" "$MAX_TIER" "$MESSAGES") || exit $?
  MAX_EXPECTED_DELIVERY_PAYLOAD_BYTES=$(
    checked_product expected-delivery-payload-bytes "$MAX_EXPECTED_DELIVERIES" "$PAYLOAD_BYTES"
  ) || exit $?
  PAYLOAD_SAMPLE=$(build_payload plan 0 1 1) || exit $?
  ((${#PAYLOAD_SAMPLE} == PAYLOAD_BYTES)) || fail 'plan payload length self-check failed'
  printf 'capacity-plan tiers=%s messages_per_publisher=%d max_clients=%d required_inflight_per_session=%d payload_bytes=%d expected_delivery_payload_bytes=%d\n' \
    "$TIERS" "$MESSAGES" "$REQUIRED_CONNECTIONS" "$REQUIRED_INFLIGHT" \
    "$PAYLOAD_BYTES" "$MAX_EXPECTED_DELIVERY_PAYLOAD_BYTES"
else
  printf 'capacity-plan tiers=%s messages_per_publisher=%d max_clients=%d required_inflight_per_session=%d\n' \
    "$TIERS" "$MESSAGES" "$REQUIRED_CONNECTIONS" "$REQUIRED_INFLIGHT"
fi

if ((PLAN_ONLY)); then
  if [[ -n "$MAX_CONNECTIONS" ]]; then
    is_positive_integer "$MAX_CONNECTIONS" || fail 'max-connections must be a positive integer'
    ((MAX_CONNECTIONS >= REQUIRED_CONNECTIONS)) ||
      fail "max-connections=$MAX_CONNECTIONS is below required=$REQUIRED_CONNECTIONS"
  fi
  if [[ -n "$MAX_INFLIGHT" ]]; then
    is_positive_integer "$MAX_INFLIGHT" || fail 'max-inflight must be a positive integer'
    ((MAX_INFLIGHT >= REQUIRED_INFLIGHT)) ||
      fail "max-inflight=$MAX_INFLIGHT is below required=$REQUIRED_INFLIGHT"
  fi
  exit 0
fi

[[ -n "$SERVER_PID" ]] || fail '--server-pid is required'
[[ -n "$SERVER_LOG" ]] || fail '--server-log is required'
[[ -n "$ARTIFACT_ROOT" ]] || fail '--artifacts is required'
[[ -n "$MAX_CONNECTIONS" ]] || fail '--max-connections is required'
[[ -n "$MAX_INFLIGHT" ]] || fail '--max-inflight is required'
is_positive_integer "$SERVER_PID" || fail 'server-pid must be a positive integer'
is_positive_integer "$MAX_CONNECTIONS" || fail 'max-connections must be a positive integer'
is_positive_integer "$MAX_INFLIGHT" || fail 'max-inflight must be a positive integer'
((MAX_CONNECTIONS >= REQUIRED_CONNECTIONS)) ||
  fail "max-connections=$MAX_CONNECTIONS is below required=$REQUIRED_CONNECTIONS"
((MAX_INFLIGHT >= REQUIRED_INFLIGHT)) ||
  fail "max-inflight=$MAX_INFLIGHT is below required=$REQUIRED_INFLIGHT"

for command in awk cat cmp date find getconf grep mosquitto_pub mosquitto_sub sed seq sort ss tee timeout touch wc; do
  command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
kill -0 "$SERVER_PID" 2>/dev/null || fail "server PID is not running: $SERVER_PID"
[[ -r "/proc/$SERVER_PID/status" ]] || fail "cannot read /proc/$SERVER_PID/status"
[[ -f "$SERVER_LOG" && -r "$SERVER_LOG" ]] || fail "server log is not readable: $SERVER_LOG"
if [[ ! "$SERVER_LOG" -ef "/proc/$SERVER_PID/fd/1" &&
      ! "$SERVER_LOG" -ef "/proc/$SERVER_PID/fd/2" ]]; then
  fail "server log does not match stdout/stderr for PID $SERVER_PID: $SERVER_LOG"
fi
[[ ! -e "$ARTIFACT_ROOT" ]] || fail "artifact path already exists: $ARTIFACT_ROOT"

cmdline_has_pair() {
  local option=$1
  local expected=$2
  local previous=
  local argument
  while IFS= read -r -d '' argument; do
    if [[ "$previous" == "$option" && "$argument" == "$expected" ]]; then
      return 0
    fi
    previous=$argument
  done < "/proc/$SERVER_PID/cmdline"
  return 1
}

cmdline_has_pair --port "$PORT" || fail "server command line does not contain --port $PORT"
cmdline_has_pair --max-connections "$MAX_CONNECTIONS" ||
  fail "server command line does not contain --max-connections $MAX_CONNECTIONS"
cmdline_has_pair --max-inflight "$MAX_INFLIGHT" ||
  fail "server command line does not contain --max-inflight $MAX_INFLIGHT"

listener_count() {
  ss -Hltn | awk -v suffix=":$PORT" '$4 ~ (suffix "$" ) { count++ } END { print count + 0 }'
}

established_count() {
  ss -Htn state established | awk -v suffix=":$PORT" '$4 ~ (suffix "$" ) { count++ } END { print count + 0 }'
}

(( $(listener_count) > 0 )) || fail "no TCP listener found on port $PORT"
mkdir -p -- "$ARTIFACT_ROOT" || fail "cannot create artifact directory: $ARTIFACT_ROOT"
SUMMARY="$ARTIFACT_ROOT/summary.csv"
printf '%s\n' \
  'tier,total_clients,published_messages,expected_deliveries,elapsed_ms,deliveries_per_second,cpu_seconds,rss_before_kb,rss_peak_kb,rss_settled_kb,vm_peak_kb,threads_peak,fds_before,fds_peak,fds_settled,connections_peak,log_lines,debug_lines,warn_lines,error_lines,fatal_lines,qos2_window_lines,result,payload_bytes,expected_delivery_payload_bytes,delivery_payload_bytes_per_second' \
  > "$SUMMARY"

sample_resources() {
  local output=$1
  local stop_file=$2
  local status values fds connections now
  while [[ -e "$stop_file" ]] && kill -0 "$SERVER_PID" 2>/dev/null; do
    status="/proc/$SERVER_PID/status"
    values=$(awk '
      /^VmRSS:/ { rss=$2 }
      /^VmSize:/ { vm=$2 }
      /^Threads:/ { threads=$2 }
      END { printf "%d,%d,%d", rss+0, vm+0, threads+0 }
    ' "$status")
    fds=$(find "/proc/$SERVER_PID/fd" -mindepth 1 -maxdepth 1 -printf . 2>/dev/null | wc -c)
    connections=$(established_count)
    now=$(date +%s%3N)
    printf '%s,%s,%s,%s\n' "$now" "$values" "$fds" "$connections" >> "$output"
    sleep 0.25
  done
}

cpu_ticks() {
  awk '{ print $14 + $15 }' "/proc/$SERVER_PID/stat"
}

record_failure() {
  TIER_FAILED=1
  printf 'FAIL: %s\n' "$*" | tee -a "$TIER_FAILURES" >&2
}

validate_payload_sequence() {
  local file=$1
  local run_id=$2
  local qos=$3
  local publishers=$4
  local expected_payload_bytes=$5
  LC_ALL=C awk -v expected_run="$run_id" -v expected_qos="$qos" \
      -v publishers="$publishers" -v messages="$MESSAGES" \
      -v expected_payload_bytes="$expected_payload_bytes" '
    BEGIN { failed=0 }
    {
      if (NF != 5) { failed=1; next }
      split($1, run_field, "=")
      split($2, qos_field, "=")
      split($3, publisher_field, "=")
      split($4, sequence_field, "=")
      marker="marker=flowie-scale-v1"
      if (run_field[1] != "run" || run_field[2] != expected_run ||
          qos_field[1] != "qos" || qos_field[2] != expected_qos ||
          publisher_field[1] != "publisher" || publisher_field[2] !~ /^[0-9]+$/ ||
          sequence_field[1] != "sequence" || sequence_field[2] !~ /^[0-9]+$/ ||
          (expected_payload_bytes == 0 && $5 != marker) ||
          (expected_payload_bytes > 0 && substr($5, 1, length(marker)) != marker)) {
        failed=1
        next
      }
      if (expected_payload_bytes > 0) {
        padding=substr($5, length(marker) + 1)
        if (length($0) != expected_payload_bytes || padding !~ /^x*$/) {
          failed=1
          next
        }
      }
      publisher=publisher_field[2] + 0
      sequence=sequence_field[2] + 0
      if (publisher < 1 || publisher > publishers || sequence != last[publisher] + 1) {
        failed=1
      }
      last[publisher]=sequence
      count[publisher]++
    }
    END {
      for (publisher=1; publisher<=publishers; publisher++) {
        if (count[publisher] != messages || last[publisher] != messages) failed=1
      }
      exit failed
    }
  ' "$file"
}

run_tier() {
  local tier=$1
  local run_id tier_dir expected_per_subscriber total_clients published deliveries
  local qos publisher sequence subscriber index pid rc ready_deadline ready_connections stable_samples
  local start_ms end_ms elapsed_ms cpu_before cpu_after clock_ticks cpu_seconds throughput publisher_gate
  local payload_bytes_value delivery_payload_bytes delivery_payload_throughput
  local start_log_line end_log_line log_delta resources
  local actual_lines sorted_actual expected_file result
  local rss_before rss_peak rss_settled vm_peak threads_peak fds_before fds_peak fds_settled connections_peak
  local log_lines debug_lines warn_lines error_lines fatal_lines qos2_window_lines expected_qos2_window_lines
  local -a sub_pids=() sub_files=() sub_names=() pub_pids=() pub_names=()

  run_id="scale-${tier}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
  tier_dir="$ARTIFACT_ROOT/tier-$tier"
  mkdir -p -- "$tier_dir/expected" "$tier_dir/inputs" "$tier_dir/subscribers" "$tier_dir/publishers"
  TIER_FAILURES="$tier_dir/failures.txt"
  : > "$TIER_FAILURES"
  TIER_FAILED=0
  expected_per_subscriber=$((tier * MESSAGES))
  total_clients=$((6 * tier))
  published=$((3 * tier * MESSAGES))
  deliveries=$((3 * tier * tier * MESSAGES))
  payload_bytes_value=${PAYLOAD_BYTES:-0}
  delivery_payload_bytes=0
  if [[ -n "$PAYLOAD_BYTES" ]]; then
    delivery_payload_bytes=$(
      checked_product expected-delivery-payload-bytes "$deliveries" "$PAYLOAD_BYTES"
    ) || return $?
  fi
  start_log_line=$(wc -l < "$SERVER_LOG")
  resources="$tier_dir/resources.csv"
  printf '%s\n' 'timestamp_ms,rss_kb,vm_kb,threads,fds,established_connections' > "$resources"
  MONITOR_STOP="$tier_dir/.monitor-running"
  : > "$MONITOR_STOP"
  sample_resources "$resources" "$MONITOR_STOP" &
  local monitor_pid=$!
  CHILD_PIDS=("$monitor_pid")
  cpu_before=$(cpu_ticks)

  for qos in 0 1 2; do
    expected_file="$tier_dir/expected/qos-$qos.txt"
    : > "$expected_file"
    for publisher in $(seq 1 "$tier"); do
      local input_file="$tier_dir/inputs/qos-${qos}-publisher-${publisher}.txt"
      : > "$input_file"
      for sequence in $(seq 1 "$MESSAGES"); do
        local payload
        payload=$(build_payload "$run_id" "$qos" "$publisher" "$sequence") || return $?
        printf '%s\n' "$payload" >> "$input_file"
        printf '%s\n' "$payload" >> "$expected_file"
      done
    done
    LC_ALL=C sort "$expected_file" -o "$expected_file"

    for subscriber in $(seq 1 "$tier"); do
      local sub_file="$tier_dir/subscribers/qos-${qos}-subscriber-${subscriber}.out"
      local sub_error="$tier_dir/subscribers/qos-${qos}-subscriber-${subscriber}.err"
      timeout --signal=TERM --kill-after=5 "$TIMEOUT_SECONDS" \
        mosquitto_sub -h "$HOST" -p "$PORT" -V mqttv5 -q "$qos" \
          -D CONNECT receive-maximum "$CLIENT_RECEIVE_MAXIMUM" \
          -i "flowie-scale-${run_id}-q${qos}-s${subscriber}" \
          -t "flowie/scale/${run_id}/${qos}/+" -C "$expected_per_subscriber" \
          > "$sub_file" 2> "$sub_error" &
      pid=$!
      sub_pids+=("$pid")
      sub_files+=("$sub_file")
      sub_names+=("qos=$qos subscriber=$subscriber")
      CHILD_PIDS+=("$pid")
    done
  done

  ready_deadline=$((SECONDS + 30))
  ready_connections=0
  while ((SECONDS < ready_deadline)); do
    ready_connections=$(established_count)
    ((ready_connections >= 3 * tier)) && break
    sleep 0.1
  done
  if ((ready_connections < 3 * tier)); then
    record_failure "subscriber readiness timeout expected=$((3 * tier)) actual=$ready_connections"
  fi
  sleep 2

  publisher_gate="$tier_dir/.publishers-ready"
  PUBLISHER_GATE=$publisher_gate
  for qos in 0 1 2; do
    for publisher in $(seq 1 "$tier"); do
      timeout --signal=TERM --kill-after=5 "$TIMEOUT_SECONDS" \
        mosquitto_pub -h "$HOST" -p "$PORT" -V mqttv5 -q "$qos" \
          -i "flowie-scale-${run_id}-q${qos}-p${publisher}" \
          -t "flowie/scale/${run_id}/${qos}/${publisher}" -l \
          < <(while [[ ! -e "$publisher_gate" ]]; do sleep 0.05; done; \
              cat "$tier_dir/inputs/qos-${qos}-publisher-${publisher}.txt") \
          > "$tier_dir/publishers/qos-${qos}-publisher-${publisher}.out" \
          2> "$tier_dir/publishers/qos-${qos}-publisher-${publisher}.err" &
      pid=$!
      pub_pids+=("$pid")
      pub_names+=("qos=$qos publisher=$publisher")
      CHILD_PIDS+=("$pid")
    done
  done

  ready_deadline=$((SECONDS + 30))
  ready_connections=0
  stable_samples=0
  while ((SECONDS < ready_deadline)); do
    ready_connections=$(established_count)
    if ((ready_connections >= total_clients)); then
      stable_samples=$((stable_samples + 1))
      ((stable_samples >= 10)) && break
    else
      stable_samples=0
    fi
    sleep 0.1
  done
  printf 'expected=%d actual=%d consecutive_samples=%d\n' \
    "$total_clients" "$ready_connections" "$stable_samples" > "$tier_dir/all-client-readiness.txt"
  if ((stable_samples < 10)); then
    record_failure "all-client readiness timeout expected=$total_clients actual=$ready_connections stable_samples=$stable_samples"
  fi
  start_ms=$(date +%s%3N)
  touch "$publisher_gate"

  for index in "${!pub_pids[@]}"; do
    wait "${pub_pids[$index]}"
    rc=$?
    ((rc == 0)) || record_failure "publisher ${pub_names[$index]} exit_code=$rc"
  done
  PUBLISHER_GATE=
  for index in "${!sub_pids[@]}"; do
    wait "${sub_pids[$index]}"
    rc=$?
    ((rc == 0)) || record_failure "subscriber ${sub_names[$index]} exit_code=$rc"
  done
  end_ms=$(date +%s%3N)
  elapsed_ms=$((end_ms - start_ms))
  sleep 2
  if kill -0 "$SERVER_PID" 2>/dev/null; then
    cpu_after=$(cpu_ticks)
  else
    cpu_after=$cpu_before
    record_failure 'server exited before final CPU sample'
  fi
  rm -f -- "$MONITOR_STOP"
  wait "$monitor_pid" 2>/dev/null || true
  CHILD_PIDS=()
  MONITOR_STOP=

  for index in "${!sub_files[@]}"; do
    actual_lines=$(wc -l < "${sub_files[$index]}")
    ((actual_lines == expected_per_subscriber)) ||
      record_failure "subscriber ${sub_names[$index]} line_count=$actual_lines expected=$expected_per_subscriber"
    qos=${sub_names[$index]#qos=}
    qos=${qos%% *}
    expected_file="$tier_dir/expected/qos-$qos.txt"
    sorted_actual="${sub_files[$index]}.sorted"
    LC_ALL=C sort "${sub_files[$index]}" > "$sorted_actual"
    cmp -s "$expected_file" "$sorted_actual" ||
      record_failure "subscriber ${sub_names[$index]} multiset mismatch"
    validate_payload_sequence "${sub_files[$index]}" "$run_id" "$qos" "$tier" \
      "$payload_bytes_value" ||
      record_failure "subscriber ${sub_names[$index]} payload/FIFO mismatch"
  done

  kill -0 "$SERVER_PID" 2>/dev/null || record_failure 'server exited during tier'
  (( $(listener_count) > 0 )) || record_failure "listener disappeared from port $PORT"

  end_log_line=$(wc -l < "$SERVER_LOG")
  log_delta="$tier_dir/server-log-delta.log"
  if ((end_log_line > start_log_line)); then
    sed -n "$((start_log_line + 1)),${end_log_line}p" "$SERVER_LOG" > "$log_delta"
  else
    : > "$log_delta"
  fi
  log_lines=$(wc -l < "$log_delta")
  debug_lines=$(grep -c '\[DEBUG\]' "$log_delta" || true)
  warn_lines=$(grep -c '\[WARN\]' "$log_delta" || true)
  error_lines=$(grep -c '\[ERROR\]' "$log_delta" || true)
  fatal_lines=$(grep -c '\[FATAL\]' "$log_delta" || true)
  qos2_window_lines=$(grep -c 'qos2-window' "$log_delta" || true)
  expected_qos2_window_lines=0
  if ((expected_per_subscriber >= CLIENT_RECEIVE_MAXIMUM)); then
    expected_qos2_window_lines=$((2 * tier))
  fi
  ((warn_lines == 0 && error_lines == 0 && fatal_lines == 0)) ||
    record_failure "log severity warn=$warn_lines error=$error_lines fatal=$fatal_lines"
  if grep -Eq '\[\][[:space:]]*(\[|\(|$)' "$log_delta"; then
    record_failure 'log contains an empty component or source field'
  fi
  if grep -Eaiq 'password|authorization|api[_-]?key|secret|token|client_id|username|payload=' "$log_delta"; then
    record_failure 'log contains a forbidden sensitive field name'
  fi
  ((qos2_window_lines == expected_qos2_window_lines)) ||
    record_failure "qos2-window debug lines=$qos2_window_lines expected=$expected_qos2_window_lines"

  read -r rss_before rss_peak rss_settled vm_peak threads_peak fds_before fds_peak fds_settled connections_peak < <(
    awk -F, 'NR == 2 { rss_before=$2; fds_before=$5 }
      NR > 1 {
        if ($2 > rss_peak) rss_peak=$2
        if ($3 > vm_peak) vm_peak=$3
        if ($4 > threads_peak) threads_peak=$4
        if ($5 > fds_peak) fds_peak=$5
        if ($6 > connections_peak) connections_peak=$6
        rss_settled=$2; fds_settled=$5
      }
      END { print rss_before+0, rss_peak+0, rss_settled+0, vm_peak+0, threads_peak+0,
                  fds_before+0, fds_peak+0, fds_settled+0, connections_peak+0 }' "$resources"
  )
  ((connections_peak >= total_clients)) ||
    record_failure "connection peak=$connections_peak expected_at_least=$total_clients"
  clock_ticks=$(getconf CLK_TCK)
  cpu_seconds=$(awk -v before="$cpu_before" -v after="$cpu_after" -v hz="$clock_ticks" \
    'BEGIN { printf "%.3f", (after-before)/hz }')
  throughput=$(awk -v count="$deliveries" -v elapsed="$elapsed_ms" \
    'BEGIN { if (elapsed == 0) print "0.000"; else printf "%.3f", count*1000/elapsed }')
  delivery_payload_throughput=$(awk -v count="$delivery_payload_bytes" -v elapsed="$elapsed_ms" \
    'BEGIN { if (elapsed == 0) print "0.000"; else printf "%.3f", count*1000/elapsed }')
  result=PASS
  ((TIER_FAILED == 0)) || result=FAIL
  printf '%d,%d,%d,%d,%d,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%s\n' \
    "$tier" "$total_clients" "$published" "$deliveries" "$elapsed_ms" "$throughput" \
    "$cpu_seconds" "$rss_before" "$rss_peak" "$rss_settled" "$vm_peak" "$threads_peak" \
    "$fds_before" "$fds_peak" "$fds_settled" "$connections_peak" "$log_lines" "$debug_lines" \
    "$warn_lines" "$error_lines" "$fatal_lines" "$qos2_window_lines" "$result" \
    "$payload_bytes_value" "$delivery_payload_bytes" "$delivery_payload_throughput" | tee -a "$SUMMARY"
  ((TIER_FAILED == 0))
}

for tier in "${TIER_VALUES[@]}"; do
  printf 'tier-start subscribers_per_qos=%d publishers_per_qos=%d total_clients=%d\n' \
    "$tier" "$tier" "$((6 * tier))"
  run_tier "$tier" || fail "tier $tier failed; see $ARTIFACT_ROOT/tier-$tier"
done

printf 'scale-load PASS summary=%s\n' "$SUMMARY"
