# The Mapper plugin uses CMeta::CSTL image-local static symbols. Neither a
# preemptible exported definition nor an unresolved dynamic import is allowed.
# This gate intentionally audits the FINAL native Mapper shared image.
if(NOT DEFINED MAPPER_PLUGIN_IMAGE OR NOT EXISTS "${MAPPER_PLUGIN_IMAGE}")
  message(FATAL_ERROR "Mapper shared image was not built: ${MAPPER_PLUGIN_IMAGE}")
endif()
if(NOT DEFINED CMAKE_NM_TOOL OR CMAKE_NM_TOOL STREQUAL "")
  message(FATAL_ERROR "Exact target nm tool is required")
endif()

foreach(_mode IN ITEMS --undefined-only --defined-only)
  execute_process(
    COMMAND "${CMAKE_NM_TOOL}" -D "${_mode}" "${MAPPER_PLUGIN_IMAGE}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _symbols
    ERROR_VARIABLE _error)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Mapper native symbols unavailable (${_mode}): ${_error}")
  endif()
  string(REPLACE "\n" ";" _lines "${_symbols}")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "^[ \t]*([0-9a-fA-F]+[ \t]+)?[A-Za-z][ \t]+hash_bytes(@.*)?$")
      message(FATAL_ERROR
        "Mapper violates image-local CSTL symbol policy (${_mode}): ${_line}")
    endif()
  endforeach()
endforeach()
message(STATUS "Mapper CSTL hash_bytes has no unresolved or exported ELF dynamic symbol")
