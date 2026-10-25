cmake_add_test(
  test_flowie_mqtt_client
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_mqtt_client_transport.c
  LIBS Flowie::Client Flowie::Protocol Flowie::Connection Salts::Core
       Salts::TinyTest
  FOLDER "client/tests")

target_include_directories(test_flowie_mqtt_client
                           PRIVATE ${PROJECT_SOURCE_DIR}/flowie/tests)
target_link_libraries(test_flowie_mqtt_client PRIVATE GmSSL::GmSSL)
set_tests_properties(
  test_flowie_mqtt_client
  PROPERTIES LABELS "flowie;flowie-release;mqtt;client;mqtt-security;mqtt-negative")

# #70: acceptance tests are activated only when the exact GmSSL/Salt SDK
# candidate has been installed and independently qualified. Do not silently
# claim LE/SHA384 compatibility using the older production SDK.
option(FLOWIE_P384_SHA384_ACCEPTANCE_TESTS
  "Enable exact P384-issuer/P256-leaf SHA384 MQTT TLS and WSS acceptance" OFF)
if(FLOWIE_P384_SHA384_ACCEPTANCE_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_program(FLOWIE_SHA384_TEST_OPENSSL NAMES openssl REQUIRED)
  set(_flowie_p384_pki "${CMAKE_CURRENT_BINARY_DIR}/p384-sha384-fixtures")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/generate_le_sha384_fixtures.py"
      --openssl "${FLOWIE_SHA384_TEST_OPENSSL}" --out "${_flowie_p384_pki}"
    RESULT_VARIABLE _flowie_p384_pki_rc
    OUTPUT_VARIABLE _flowie_p384_pki_output
    ERROR_VARIABLE _flowie_p384_pki_error)
  if(NOT _flowie_p384_pki_rc EQUAL 0)
    message(FATAL_ERROR
      "Flowie P384/SHA384 independent test PKI generation failed:"
      "\n${_flowie_p384_pki_output}\n${_flowie_p384_pki_error}")
  endif()
  foreach(_fixture IN ITEMS ca.pem leaf.pem dns-only-leaf.pem expired-leaf.pem
                            future-leaf.pem leaf.key wrong-ca.pem tampered.pem)
    if(NOT EXISTS "${_flowie_p384_pki}/${_fixture}")
      message(FATAL_ERROR "Missing Flowie P384/SHA384 test certificate material: ${_fixture}")
    endif()
  endforeach()
  file(TO_CMAKE_PATH "${_flowie_p384_pki}/ca.pem" _p384_ca)
  file(TO_CMAKE_PATH "${_flowie_p384_pki}/leaf.pem" _p384_leaf)
  file(TO_CMAKE_PATH "${_flowie_p384_pki}/dns-only-leaf.pem" _p384_dns_only_leaf)
  file(TO_CMAKE_PATH "${_flowie_p384_pki}/leaf.key" _p384_key)
  file(TO_CMAKE_PATH "${_flowie_p384_pki}/wrong-ca.pem" _p384_wrong_ca)
  file(TO_CMAKE_PATH "${_flowie_p384_pki}/tampered.pem" _p384_tampered)
  file(TO_CMAKE_PATH "${_flowie_p384_pki}/expired-leaf.pem" _p384_expired)
  file(TO_CMAKE_PATH "${_flowie_p384_pki}/future-leaf.pem" _p384_future)
  target_compile_definitions(test_flowie_mqtt_client PRIVATE
    FLOWIE_P384_SHA384_ACCEPTANCE_TESTS=1
    "FLOWIE_P384_SHA384_CA=\"${_p384_ca}\""
    "FLOWIE_P384_SHA384_LEAF=\"${_p384_leaf}\""
    "FLOWIE_P384_SHA384_DNS_ONLY_LEAF=\"${_p384_dns_only_leaf}\""
    "FLOWIE_P384_SHA384_KEY=\"${_p384_key}\""
    "FLOWIE_P384_SHA384_WRONG_CA=\"${_p384_wrong_ca}\""
    "FLOWIE_P384_SHA384_TAMPERED_LEAF=\"${_p384_tampered}\""
    "FLOWIE_P384_SHA384_EXPIRED_LEAF=\"${_p384_expired}\""
    "FLOWIE_P384_SHA384_FUTURE_LEAF=\"${_p384_future}\"")
  message(STATUS "Flowie #70 P384/SHA384 MQTT TLS+WSS acceptance enabled; GmSSL remains private CNet runtime")
endif()

cmake_add_test(
  test_flowie_mqtt_client_live
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_mqtt_client_live.c
  LIBS Flowie::Client Flowie::Protocol Salts::Core Salts::TinyTest
  FOLDER "client/tests")

set_tests_properties(
  test_flowie_mqtt_client_live
  PROPERTIES LABELS "flowie;mqtt;client;integration;mqtt-public-live;external-smoke"
             TIMEOUT 120)
if(NOT FLOWIE_MQTT_PUBLIC_LIVE_TESTS)
  set_tests_properties(test_flowie_mqtt_client_live PROPERTIES DISABLED TRUE)
endif()

if(FLOWIE_MQTT_FIXED_INTEROP_TESTS)
  if(NOT FLOWIE_MQTT_FIXED_CA_FILE OR NOT EXISTS "${FLOWIE_MQTT_FIXED_CA_FILE}")
    message(FATAL_ERROR
            "FLOWIE_MQTT_FIXED_INTEROP_TESTS requires FLOWIE_MQTT_FIXED_CA_FILE")
  endif()
  cmake_add_test(
    test_flowie_mqtt_client_fixed_interop
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_mqtt_client_live.c
    LIBS Flowie::Client Flowie::Protocol Salts::Core Salts::TinyTest
    FOLDER "client/tests")
  target_compile_definitions(
    test_flowie_mqtt_client_fixed_interop
    PRIVATE FLOWIE_MQTT_FIXED_INTEROP=1
            FLOWIE_MQTT_FIXED_BROKER_NAME="${FLOWIE_MQTT_FIXED_BROKER_NAME}"
            FLOWIE_MQTT_FIXED_HOST="${FLOWIE_MQTT_FIXED_HOST}"
            FLOWIE_MQTT_FIXED_TCP_PORT=${FLOWIE_MQTT_FIXED_TCP_PORT}
            FLOWIE_MQTT_FIXED_TLS_PORT=${FLOWIE_MQTT_FIXED_TLS_PORT}
            FLOWIE_MQTT_FIXED_WS_PORT=${FLOWIE_MQTT_FIXED_WS_PORT}
            FLOWIE_MQTT_FIXED_WSS_PORT=${FLOWIE_MQTT_FIXED_WSS_PORT}
            FLOWIE_MQTT_FIXED_WS_PATH="${FLOWIE_MQTT_FIXED_WS_PATH}"
            FLOWIE_MQTT_FIXED_CA_FILE="${FLOWIE_MQTT_FIXED_CA_FILE}"
            FLOWIE_MQTT_FIXED_SUPPORT_31=$<BOOL:${FLOWIE_MQTT_FIXED_SUPPORT_31}>
            FLOWIE_MQTT_FIXED_SUPPORT_31_WS=$<BOOL:${FLOWIE_MQTT_FIXED_SUPPORT_31_WS}>)
  set_tests_properties(
    test_flowie_mqtt_client_fixed_interop
    PROPERTIES LABELS
               "flowie;flowie-release;mqtt-interop;mqtt-fixed-interop;mqtt-persistence"
               TIMEOUT 300)
  add_test(
    NAME flowie_mqtt_fixed_mqtt31_capability
    COMMAND
      ${CMAKE_COMMAND} -E echo
      "$<IF:$<BOOL:${FLOWIE_MQTT_FIXED_SUPPORT_31}>,SUPPORTED: MQTT 3.1 TCP/TLS,SKIP: fixed broker does not declare MQTT 3.1 TCP/TLS>")
  set_tests_properties(
    flowie_mqtt_fixed_mqtt31_capability
    PROPERTIES LABELS "flowie;flowie-release;mqtt-interop;mqtt-fixed-interop"
               SKIP_REGULAR_EXPRESSION "^SKIP: fixed broker")
  add_test(
    NAME flowie_mqtt_fixed_mqtt31_ws_capability
    COMMAND
      ${CMAKE_COMMAND} -E echo
      "$<IF:$<BOOL:${FLOWIE_MQTT_FIXED_SUPPORT_31_WS}>,SUPPORTED: MQTT 3.1 WS/WSS,SKIP: fixed broker does not declare MQTT 3.1 WS/WSS>")
  set_tests_properties(
    flowie_mqtt_fixed_mqtt31_ws_capability
    PROPERTIES LABELS "flowie;flowie-release;mqtt-interop;mqtt-fixed-interop"
               SKIP_REGULAR_EXPRESSION "^SKIP: fixed broker")
  if(WIN32)
    target_link_libraries(test_flowie_mqtt_client_fixed_interop PRIVATE ws2_32)
  endif()
endif()

if(WIN32)
  target_link_libraries(test_flowie_mqtt_client PRIVATE ws2_32)
  target_link_libraries(test_flowie_mqtt_client_live PRIVATE ws2_32)
endif()

# Compile the exact Client translation unit as a separate test-only DLL so
# deterministic CNet admission failures never leak into the installed SDK.
if(FLOWIE_BUILD_TESTS)
  add_library(flowie_client_close_fault SHARED
              ${PROJECT_SOURCE_DIR}/client/src/flowie_mqtt_client.c)
  target_compile_definitions(flowie_client_close_fault
                             PRIVATE FLOWIE_MQTT_CLIENT_BUILD FLOWIE_CLIENT_FAULT_TEST)
  target_include_directories(
    flowie_client_close_fault
    PUBLIC ${PROJECT_SOURCE_DIR}/client/include
           ${PROJECT_SOURCE_DIR}/protocol/include
    PRIVATE ${PROJECT_SOURCE_DIR}/flowie/include)
  target_link_libraries(
    flowie_client_close_fault
    PRIVATE Flowie::Protocol Salts::Core Salts::CSTL Salts::CNet CHttp::Client
            monocypher)

  cmake_add_test(
    test_flowie_mqtt_client_close_full
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_mqtt_client_transport.c
    LIBS flowie_client_close_fault Flowie::Protocol Flowie::Connection
         Salts::Core Salts::TinyTest
    FOLDER "client/tests")
  target_compile_definitions(test_flowie_mqtt_client_close_full
                             PRIVATE FLOWIE_CLIENT_FAULT_TEST)
  target_include_directories(test_flowie_mqtt_client_close_full
                             PRIVATE ${PROJECT_SOURCE_DIR}/flowie/tests)
  target_link_libraries(test_flowie_mqtt_client_close_full
                        PRIVATE GmSSL::GmSSL)
  if(WIN32)
    target_link_libraries(test_flowie_mqtt_client_close_full PRIVATE ws2_32)
  endif()
  set_tests_properties(test_flowie_mqtt_client_close_full PROPERTIES
    LABELS "flowie;flowie-release;mqtt;client;fault-injection;lifetime"
    TIMEOUT 60)
endif()
