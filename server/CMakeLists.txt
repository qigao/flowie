add_executable(flowie_server flowie_server.c)
target_link_libraries(flowie_server PRIVATE Flowie::App)
install(TARGETS flowie_server RUNTIME DESTINATION bin)

if(FLOWIE_BUILD_TESTS)
  add_test(NAME flowie_server_check_debug_logging
           COMMAND flowie_server --check --log-level DEBUG --max-inflight 17)
  set_tests_properties(
    flowie_server_check_debug_logging
    PROPERTIES
      LABELS "flowie;flowie-release;product-contract;diagnostics"
      PASS_REGULAR_EXPRESSION
        "\\[DEBUG\\].*\\[Flowie.Server\\].*effective-config.*max_inflight_per_session=17")

  add_test(NAME flowie_server_check_logging_shutdown_snapshot
           COMMAND flowie_server --check --log-level INFO)
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
