add_executable(
  flowie_server
  flowie_server.c
  flowie_server_config.c
  flowie_server_http_security.c
  flowie_server_runtime.c
  flowie_server_turbodb_config.c)
target_link_libraries(
  flowie_server PRIVATE Flowie::App Orm::C CHttp::Client Salts::CmdParser
                        Salts::CYaml Salts::JsonParser Salts::UriParser
                        ${FLOWIE_SALTS_COMPONENT_TARGET} Salts::CMeta)
target_compile_definitions(flowie_server PRIVATE ${FLOWIE_SALTS_COMPONENT_DEFINITION})
install(TARGETS flowie_server RUNTIME DESTINATION bin)

if(FLOWIE_BUILD_TESTS)
  cmake_add_test(
    test_flowie_server_runtime
    SOURCES tests/test_flowie_server_runtime.c flowie_server_runtime.c
            flowie_server_config.c flowie_server_http_security.c flowie_server_turbodb_config.c
    LIBS Flowie::App Orm::C CHttp::Client ${FLOWIE_SALTS_COMPONENT_TARGET} Salts::CMeta
         Salts::CYaml Salts::JsonParser Salts::UriParser Salts::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/include
             ${PROJECT_SOURCE_DIR}/flowie/tests
    DEFS FLOWIE_TEST_MULTI_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/flowie_server_network_workers.yml"
    FOLDER "flowie/server/tests")
  target_compile_definitions(test_flowie_server_runtime
                             PRIVATE ${FLOWIE_SALTS_COMPONENT_DEFINITION})
  set_tests_properties(test_flowie_server_runtime PROPERTIES
      LABELS "flowie;flowie-release;product-contract;ownership;configurator;cnet")

  cmake_add_test(
    test_flowie_server_config
    SOURCES tests/test_flowie_server_config.c flowie_server_config.c
    LIBS Flowie::Protocol Orm::C Salts::CYaml Salts::Core Salts::CFlow
         Salts::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/include
    FOLDER "flowie/server/tests")
  target_compile_definitions(
    test_flowie_server_config
    PRIVATE FLOWIE_TEST_EU_CONFIG="${PROJECT_SOURCE_DIR}/deploy/server/.remote-flowie-eu.yml"
            FLOWIE_TEST_INSECURE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/flowie_server_no_auth_provider.yml"
            FLOWIE_TEST_MULTI_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/flowie_server_network_workers.yml")
  set_tests_properties(
    test_flowie_server_config
    PROPERTIES LABELS "flowie;flowie-release;product-contract;security-contract")

  cmake_add_test(
    test_flowie_server_http_security
    SOURCES tests/test_flowie_server_http_security.c flowie_server_http_security.c
    LIBS Flowie::Protocol Orm::C CHttp::Client CHttp::Server Salts::JsonParser Salts::UriParser
         Salts::Core Salts::CMeta Salts::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/include
             ${PROJECT_SOURCE_DIR}/flowie/tests
    FOLDER "flowie/server/tests")
  set_tests_properties(
    test_flowie_server_http_security
    PROPERTIES LABELS "flowie;flowie-release;product-contract;security-contract")
  if(WIN32)
    set_tests_properties(
      test_flowie_server_http_security
      PROPERTIES ENVIRONMENT_MODIFICATION
                 "PATH=path_list_prepend:$<TARGET_FILE_DIR:CHttp::Client>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  endif()

  add_test(
    NAME flowie_server_check_native_https_security
    COMMAND flowie_server --check --require-security
            --config ${PROJECT_SOURCE_DIR}/deploy/server/.remote-flowie-eu.yml
            --profile flowie --protocol-store-driver sqlite
            --protocol-store-options {"filename":":memory:"})
  set_tests_properties(
    flowie_server_check_native_https_security
    PROPERTIES
      LABELS "flowie;flowie-release;product-contract;security-contract;cnet"
      ENVIRONMENT "FLOWIE_AUTH_SERVICE_TOKEN=test-only-token")

  cmake_add_test(
    test_flowie_server_turbodb_config
    SOURCES tests/test_flowie_server_turbodb_config.c flowie_server_turbodb_config.c
    LIBS Orm::C Salts::CFlow Salts::JsonParser Salts::TinyTest
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

endif()
