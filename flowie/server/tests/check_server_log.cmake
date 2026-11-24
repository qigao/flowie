cmake_minimum_required(VERSION 3.27)

math(EXPR program_index "${CMAKE_ARGC} - 1")
set(program "${CMAKE_ARGV${program_index}}")
set(args --check --protocol-store-driver sqlite --protocol-store-options
  [=[{"filename":":memory:"}]=] --log-level "${LOG_LEVEL}")
if(DEFINED MAX_INFLIGHT)
  list(APPEND args --max-inflight "${MAX_INFLIGHT}")
endif()

execute_process(COMMAND "${program}" ${args}
  RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
message("${stdout}${stderr}")
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "flowie_server --check failed: ${result}")
endif()
if(NOT "${stdout}\n${stderr}" MATCHES "${EXPECT}")
  message(FATAL_ERROR "Missing expected server log: ${EXPECT}")
endif()
