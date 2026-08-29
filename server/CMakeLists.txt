add_executable(flowie_server flowie_server.c flowie_server_turbodb_config.c)
target_link_libraries(flowie_server PRIVATE Flowie::App Orm::C TurboParser::Parser)
install(TARGETS flowie_server RUNTIME DESTINATION bin)

if(FLOWIE_BUILD_TESTS)
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
