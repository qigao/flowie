if(NOT DEFINED FLOWIE_SERVER_EXECUTABLE OR FLOWIE_SERVER_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "FLOWIE_SERVER_EXECUTABLE is required")
endif()
if(NOT DEFINED FLOWIE_TUNING_ERROR_CASE)
  message(FATAL_ERROR "FLOWIE_TUNING_ERROR_CASE is required")
endif()

if(FLOWIE_TUNING_ERROR_CASE STREQUAL "coroutine-stack")
  set(_arguments --check --coroutine-stack-size 65535)
elseif(FLOWIE_TUNING_ERROR_CASE STREQUAL "keepalive-dependency")
  set(_arguments --check --tcp-keepalive-idle-ms 1)
else()
  message(FATAL_ERROR "unknown FLOWIE_TUNING_ERROR_CASE=${FLOWIE_TUNING_ERROR_CASE}")
endif()

execute_process(
  COMMAND "${FLOWIE_SERVER_EXECUTABLE}" ${_arguments}
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr)
set(_output "${_stdout}${_stderr}")
if(_result EQUAL 0)
  message(FATAL_ERROR "invalid tuning was accepted: ${FLOWIE_TUNING_ERROR_CASE}")
endif()
string(FIND "${_output}" "flowie_server: invalid listener options" _position)
if(_position EQUAL -1)
  message(FATAL_ERROR
          "invalid tuning was not rejected at the CLI boundary (${FLOWIE_TUNING_ERROR_CASE}):\n${_output}")
endif()

message(STATUS "standalone tuning rejection contract: PASS (${FLOWIE_TUNING_ERROR_CASE})")
