if(NOT DEFINED INPUT OR NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "INPUT must name an existing generated text file")
endif()
if(NOT DEFINED OUTPUT OR "${OUTPUT}" STREQUAL "")
  message(FATAL_ERROR "OUTPUT is required")
endif()
if(NOT DEFINED SYMBOL OR NOT SYMBOL MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
  message(FATAL_ERROR "SYMBOL must be a C identifier")
endif()
if(NOT DEFINED HEADER OR "${HEADER}" STREQUAL "")
  message(FATAL_ERROR "HEADER is required")
endif()
if(NOT DEFINED FUNCTION OR NOT FUNCTION MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
  message(FATAL_ERROR "FUNCTION must be a C identifier")
endif()

file(READ "${INPUT}" _embedded_text HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _embedded_bytes "${_embedded_text}")
file(WRITE "${OUTPUT}"
     "#include \"${HEADER}\"\n\n"
     "static const unsigned char ${SYMBOL}[] = {${_embedded_bytes}0x00};\n\n"
     "const char *${FUNCTION}(size_t *size_out) {\n"
     "  if (size_out) *size_out = sizeof(${SYMBOL}) - 1u;\n"
     "  return (const char *)${SYMBOL};\n"
     "}\n")
