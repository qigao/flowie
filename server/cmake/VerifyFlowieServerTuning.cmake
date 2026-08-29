if(NOT DEFINED FLOWIE_SERVER_EXECUTABLE OR FLOWIE_SERVER_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "FLOWIE_SERVER_EXECUTABLE is required")
endif()

execute_process(
  COMMAND "${FLOWIE_SERVER_EXECUTABLE}"
          --check
          --protocol-store-driver sqlite
          --protocol-store-options "{\"filename\":\":memory:\"}"
          --log-level DEBUG
          --max-packet-size 262144
          --max-connections 32
          --max-sessions 48
          --max-subscriptions-per-session 33
          --max-inflight 17
          --max-retained-messages 40
          --send-hwm-bytes 131072
          --coroutine-stack-size 65536
          --stream-recv-buffer-bytes 8192
          --socket-recv-buffer-bytes 32768
          --socket-send-buffer-bytes 65536
          --timeout-ms 30000
          --recv-timeout-ms 15000
          --tcp-keepalive
          --tcp-keepalive-idle-ms 60000
          --tcp-keepalive-interval-ms 10000
          --tcp-keepalive-count 3
          --reuse-port
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr)

set(_output "${_stdout}${_stderr}")
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "custom tuning was rejected (${_result}):\n${_output}")
endif()

foreach(_expected IN ITEMS
    "max_packet_size=262144"
    "max_connections=32"
    "max_sessions=48"
    "max_subscriptions_per_session=33"
    "max_inflight_per_session=17"
    "max_retained_messages=40"
    "send_hwm_bytes=131072"
    "coroutine_stack_size=65536"
    "stream_recv_buffer_bytes=8192"
    "socket_recv_buffer_bytes=32768"
    "socket_send_buffer_bytes=65536"
    "timeout_ms=30000"
    "recv_timeout_ms=15000"
    "tcp_keepalive=1"
    "tcp_keepalive_idle_ms=60000"
    "tcp_keepalive_interval_ms=10000"
    "tcp_keepalive_count=3"
    "reuse_port=1")
  string(FIND "${_output}" "${_expected}" _position)
  if(_position EQUAL -1)
    message(FATAL_ERROR "effective config is missing ${_expected}:\n${_output}")
  endif()
endforeach()

message(STATUS "standalone custom tuning contract: PASS")
