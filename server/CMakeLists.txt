add_executable(
  flowie_server
  flowie_server.c
  flowie_server_config.c
  flowie_server_http_security.c
  flowie_server_turbodb_config.c)
target_link_libraries(
  flowie_server PRIVATE Flowie::App Orm::C TurboHttp::TurboHttp TurboParser::Parser)
cmake_target_enable_boringssl_linkage(flowie_server)
install(TARGETS flowie_server RUNTIME DESTINATION bin)

if(FLOWIE_BUILD_TESTS)
  cmake_add_test(
    test_flowie_server_config
    SOURCES tests/test_flowie_server_config.c flowie_server_config.c
    LIBS Flowie::Protocol Orm::C TurboNet::CoroNet TurboParser::Parser TurboUtils::Core
         TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/include
    FOLDER "flowie/server/tests")
  target_compile_definitions(
    test_flowie_server_config
    PRIVATE FLOWIE_TEST_EU_CONFIG="${PROJECT_SOURCE_DIR}/deploy/server/.remote-flowie-eu.yml"
            FLOWIE_TEST_INSECURE_CONFIG="${PROJECT_SOURCE_DIR}/flowie/examples/flowie.yml")
  set_tests_properties(
    test_flowie_server_config
    PROPERTIES LABELS "flowie;flowie-release;product-contract;security-contract")

  cmake_add_test(
    test_flowie_server_http_security
    SOURCES tests/test_flowie_server_http_security.c flowie_server_http_security.c
    LIBS Flowie::Protocol Orm::C TurboHttp::TurboHttp TurboNet::CoroNet TurboParser::Parser
         TurboUtils::Core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/include
    FOLDER "flowie/server/tests")
  set_tests_properties(
    test_flowie_server_http_security
    PROPERTIES LABELS "flowie;flowie-release;product-contract;security-contract")
  cmake_target_enable_boringssl_linkage(test_flowie_server_http_security)

  add_test(
    NAME flowie_server_check_native_https_security
    COMMAND flowie_server --check --require-security
            --config ${PROJECT_SOURCE_DIR}/deploy/server/.remote-flowie-eu.yml
            --profile flowie --protocol-store-driver sqlite
            --protocol-store-options {"filename":":memory:"})
  set_tests_properties(
    flowie_server_check_native_https_security
    PROPERTIES
      LABELS "flowie;flowie-release;product-contract;security-contract;coronet"
      ENVIRONMENT "FLOWIE_AUTH_SERVICE_TOKEN=test-only-token")

  cmake_add_test(
    test_flowie_server_turbodb_config
    SOURCES tests/test_flowie_server_turbodb_config.c flowie_server_turbodb_config.c
    LIBS Orm::C TurboParser::Parser TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/server/tests")
  set_tests_properties(
    test_flowie_server_turbodb_config
    PROPERTIES LABELS "flowie;flowie-release;product-contract;persistence;turbodb")

  add_test(NAME flowie_server_check_debug_logging
           COMMAND flowie_server --check --protocol-store-driver sqlite
                   --protocol-store-options {"filename":":memory:"} --log-level DEBUG
                   --max-inflight 17)
  set_tests_properties(
    flowie_server_check_debug_logging
    PROPERTIES
      LABELS "flowie;flowie-release;product-contract;diagnostics"
      PASS_REGULAR_EXPRESSION
        "\\[DEBUG\\].*\\[Flowie.Server\\].*effective-config.*max_inflight_per_session=17")

  add_test(NAME flowie_server_check_logging_shutdown_snapshot
           COMMAND flowie_server --check --protocol-store-driver sqlite
                   --protocol-store-options {"filename":":memory:"} --log-level INFO)
  set_tests_properties(
    flowie_server_check_logging_shutdown_snapshot
    PROPERTIES
      LABELS "flowie;flowie-release;product-contract;diagnostics"
      PASS_REGULAR_EXPRESSION
        "logging-shutdown written_before_summary=[0-9]+ dropped_total=0 pending_before_summary=0")

  add_test(
    NAME flowie_server_check_custom_tuning
    COMMAND
      ${CMAKE_COMMAND}
      -DFLOWIE_SERVER_EXECUTABLE=$<TARGET_FILE:flowie_server>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFlowieServerTuning.cmake)
  set_tests_properties(
    flowie_server_check_custom_tuning
    PROPERTIES LABELS "flowie;flowie-release;product-contract;capacity;diagnostics")

  add_test(
    NAME flowie_server_uses_turbodb
    COMMAND
      ${CMAKE_COMMAND}
      -DFLOWIE_SERVER_EXECUTABLE=$<TARGET_FILE:flowie_server>
      -DFLOWIE_SERVER_PROTOCOL_STORE=${CMAKE_CURRENT_BINARY_DIR}/flowie-server-protocol-test.sqlite3
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFlowieServerTurboDB.cmake)
  set_tests_properties(
    flowie_server_uses_turbodb
    PROPERTIES LABELS "flowie;flowie-release;product-contract;persistence;turbodb")

  foreach(_tuning_error_case IN ITEMS coroutine-stack keepalive-dependency)
    add_test(
      NAME flowie_server_rejects_${_tuning_error_case}
      COMMAND
        ${CMAKE_COMMAND}
        -DFLOWIE_SERVER_EXECUTABLE=$<TARGET_FILE:flowie_server>
        -DFLOWIE_TUNING_ERROR_CASE=${_tuning_error_case}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFlowieServerTuningError.cmake)
    set_tests_properties(
      flowie_server_rejects_${_tuning_error_case}
      PROPERTIES LABELS "flowie;flowie-release;product-contract;capacity")
  endforeach()
endif()
