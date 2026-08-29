if(NOT DEFINED FLOWIE_SERVER_EXECUTABLE OR NOT DEFINED FLOWIE_SERVER_PROTOCOL_STORE)
  message(FATAL_ERROR "flowie server executable and protocol store are required")
endif()

file(REMOVE "${FLOWIE_SERVER_PROTOCOL_STORE}")
file(TO_CMAKE_PATH "${FLOWIE_SERVER_PROTOCOL_STORE}" _flowie_server_protocol_store)
set(_flowie_server_protocol_options
    "{\"filename\":\"${_flowie_server_protocol_store}\"}")

execute_process(
  COMMAND "${FLOWIE_SERVER_EXECUTABLE}" --check --protocol-store-driver sqlite
          --protocol-store-options "${_flowie_server_protocol_options}"
  RESULT_VARIABLE _flowie_server_result
  OUTPUT_VARIABLE _flowie_server_stdout
  ERROR_VARIABLE _flowie_server_stderr)

if(NOT _flowie_server_result EQUAL 0)
  message(FATAL_ERROR
          "flowie_server TurboDB check failed (${_flowie_server_result})\n"
          "stdout:\n${_flowie_server_stdout}\n"
          "stderr:\n${_flowie_server_stderr}")
endif()

if(NOT _flowie_server_stdout MATCHES "flowie_server: options are valid")
  message(FATAL_ERROR "flowie_server did not report a successful check")
endif()

if(NOT EXISTS "${FLOWIE_SERVER_PROTOCOL_STORE}")
  message(FATAL_ERROR "flowie_server did not create the TurboDB protocol store")
endif()

file(SIZE "${FLOWIE_SERVER_PROTOCOL_STORE}" _flowie_server_store_size)
if(_flowie_server_store_size EQUAL 0)
  message(FATAL_ERROR "flowie_server created an empty TurboDB protocol store")
endif()

execute_process(
  COMMAND "${FLOWIE_SERVER_EXECUTABLE}" --check --protocol-store-driver sqlite
          --protocol-store-options "${_flowie_server_protocol_options}"
  RESULT_VARIABLE _flowie_server_reopen_result
  OUTPUT_VARIABLE _flowie_server_reopen_stdout
  ERROR_VARIABLE _flowie_server_reopen_stderr)
if(NOT _flowie_server_reopen_result EQUAL 0)
  message(FATAL_ERROR
          "flowie_server could not reopen its TurboDB protocol store "
          "(${_flowie_server_reopen_result})\n"
          "stdout:\n${_flowie_server_reopen_stdout}\n"
          "stderr:\n${_flowie_server_reopen_stderr}")
endif()

get_filename_component(_flowie_server_store_directory "${FLOWIE_SERVER_PROTOCOL_STORE}" DIRECTORY)
file(TO_CMAKE_PATH "${_flowie_server_store_directory}" _flowie_server_store_directory_json)
set(_flowie_server_invalid_options
    "{\"filename\":\"${_flowie_server_store_directory_json}\"}")
execute_process(
  COMMAND "${FLOWIE_SERVER_EXECUTABLE}" --check --protocol-store-driver sqlite
          --protocol-store-options "${_flowie_server_invalid_options}"
  RESULT_VARIABLE _flowie_server_invalid_store_result
  OUTPUT_VARIABLE _flowie_server_invalid_store_stdout
  ERROR_VARIABLE _flowie_server_invalid_store_stderr)
if(_flowie_server_invalid_store_result EQUAL 0)
  message(FATAL_ERROR "flowie_server fell back after an invalid TurboDB protocol store")
endif()
if(NOT _flowie_server_invalid_store_stderr MATCHES "protocol-store-open-failed")
  message(FATAL_ERROR
          "flowie_server did not diagnose the invalid TurboDB protocol store\n"
          "stdout:\n${_flowie_server_invalid_store_stdout}\n"
          "stderr:\n${_flowie_server_invalid_store_stderr}")
endif()

file(REMOVE "${FLOWIE_SERVER_PROTOCOL_STORE}")
