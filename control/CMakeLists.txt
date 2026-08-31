cmake_add_grammar(FLOWIE_CONTROL_ACL
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/acl_lexer.re
  GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/acl_grammar.y
  LEXER_DEPENDS_ON_GRAMMAR
  FOLDER "flowie/control/acl")

unset(FLOWIE_CONTROL_TBE_COMPILER CACHE)
find_program(FLOWIE_CONTROL_TBE_COMPILER
             NAMES tbe_compiler tbe_compiler.exe
             PATHS "$ENV{TURBOPARSER_ROOT}/bin"
             NO_DEFAULT_PATH REQUIRED)
set(FLOWIE_CONTROL_DATA_SCHEMA
    "${CMAKE_CURRENT_SOURCE_DIR}/schema/flowie_control_data.schema")
set(FLOWIE_CONTROL_DATA_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/data-schema")
set(FLOWIE_CONTROL_DATA_SQLITE_DDL
    "${FLOWIE_CONTROL_DATA_GENERATED_DIR}/flowie-control-data.sqlite.sql")
set(FLOWIE_CONTROL_DATA_POSTGRESQL_DDL
    "${FLOWIE_CONTROL_DATA_GENERATED_DIR}/flowie-control-data.postgresql.sql")
set(FLOWIE_CONTROL_DATA_SCHEMA_SOURCE
    "${FLOWIE_CONTROL_DATA_GENERATED_DIR}/flowie_control_data_schema.c")
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${FLOWIE_CONTROL_DATA_GENERATED_DIR}"
  COMMAND "${FLOWIE_CONTROL_TBE_COMPILER}" "${FLOWIE_CONTROL_DATA_SCHEMA}"
          --lang sqlite --output "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
  DEPENDS "${FLOWIE_CONTROL_DATA_SCHEMA}" "${FLOWIE_CONTROL_TBE_COMPILER}"
  COMMENT "Generating Flowie Control SQLite data-manifest schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_DATA_POSTGRESQL_DDL}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${FLOWIE_CONTROL_DATA_GENERATED_DIR}"
  COMMAND "${FLOWIE_CONTROL_TBE_COMPILER}" "${FLOWIE_CONTROL_DATA_SCHEMA}"
          --lang postgresql --output "${FLOWIE_CONTROL_DATA_POSTGRESQL_DDL}"
  DEPENDS "${FLOWIE_CONTROL_DATA_SCHEMA}" "${FLOWIE_CONTROL_TBE_COMPILER}"
  COMMENT "Generating Flowie Control PostgreSQL data-manifest schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_DATA_SCHEMA_SOURCE}"
  COMMAND "${CMAKE_COMMAND}"
          -DINPUT=${FLOWIE_CONTROL_DATA_SQLITE_DDL}
          -DOUTPUT=${FLOWIE_CONTROL_DATA_SCHEMA_SOURCE}
          -DSYMBOL=FLOWIE_CONTROL_DATA_SCHEMA_BYTES
          -DHEADER=flowie_control_data_schema_internal.h
          -DFUNCTION=flowie_control_data_schema_sql
          -P "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  DEPENDS "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
          "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  COMMENT "Embedding Flowie Control data-manifest schema"
  VERBATIM)
add_custom_target(flowie_control_data_ddl
                  DEPENDS "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
                          "${FLOWIE_CONTROL_DATA_POSTGRESQL_DDL}")
set_target_properties(flowie_control_data_ddl PROPERTIES FOLDER "flowie/control/data")

set(FLOWIE_CONTROL_STORE_SCHEMA
    "${CMAKE_CURRENT_SOURCE_DIR}/schema/flowie_control_store.schema")
set(FLOWIE_CONTROL_STORE_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/store-schema")
set(FLOWIE_CONTROL_STORE_SQLITE_DDL
    "${FLOWIE_CONTROL_STORE_GENERATED_DIR}/flowie-control-store.sqlite.sql")
set(FLOWIE_CONTROL_STORE_POSTGRESQL_DDL
    "${FLOWIE_CONTROL_STORE_GENERATED_DIR}/flowie-control-store.postgresql.sql")
set(FLOWIE_CONTROL_STORE_SQLITE_SOURCE
    "${FLOWIE_CONTROL_STORE_GENERATED_DIR}/flowie_control_store_schema_sqlite.c")
set(FLOWIE_CONTROL_STORE_POSTGRESQL_SOURCE
    "${FLOWIE_CONTROL_STORE_GENERATED_DIR}/flowie_control_store_schema_postgresql.c")
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${FLOWIE_CONTROL_STORE_GENERATED_DIR}"
  COMMAND "${FLOWIE_CONTROL_TBE_COMPILER}" "${FLOWIE_CONTROL_STORE_SCHEMA}"
          --lang sqlite --output "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
  DEPENDS "${FLOWIE_CONTROL_STORE_SCHEMA}" "${FLOWIE_CONTROL_TBE_COMPILER}"
  COMMENT "Generating Flowie Control SQLite store schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${FLOWIE_CONTROL_STORE_GENERATED_DIR}"
  COMMAND "${FLOWIE_CONTROL_TBE_COMPILER}" "${FLOWIE_CONTROL_STORE_SCHEMA}"
          --lang postgresql --output "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}"
  DEPENDS "${FLOWIE_CONTROL_STORE_SCHEMA}" "${FLOWIE_CONTROL_TBE_COMPILER}"
  COMMENT "Generating Flowie Control PostgreSQL store schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_STORE_SQLITE_SOURCE}"
  COMMAND "${CMAKE_COMMAND}"
          -DINPUT=${FLOWIE_CONTROL_STORE_SQLITE_DDL}
          -DOUTPUT=${FLOWIE_CONTROL_STORE_SQLITE_SOURCE}
          -DSYMBOL=FLOWIE_CONTROL_STORE_SQLITE_SCHEMA_BYTES
          -DHEADER=flowie_control_store_schema_internal.h
          -DFUNCTION=flowie_control_store_schema_sqlite_sql
          -P "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  DEPENDS "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
          "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  COMMENT "Embedding Flowie Control SQLite store schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_STORE_POSTGRESQL_SOURCE}"
  COMMAND "${CMAKE_COMMAND}"
          -DINPUT=${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}
          -DOUTPUT=${FLOWIE_CONTROL_STORE_POSTGRESQL_SOURCE}
          -DSYMBOL=FLOWIE_CONTROL_STORE_POSTGRESQL_SCHEMA_BYTES
          -DHEADER=flowie_control_store_schema_internal.h
          -DFUNCTION=flowie_control_store_schema_postgresql_sql
          -P "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  DEPENDS "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}"
          "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  COMMENT "Embedding Flowie Control PostgreSQL store schema"
  VERBATIM)
add_custom_target(flowie_control_store_ddl
                  DEPENDS "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
                          "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}")
set_target_properties(flowie_control_store_ddl PROPERTIES FOLDER "flowie/control/data")

add_library(flowie_control_core STATIC ${FLOWIE_CONTROL_ACL_LEXER_GEN}
                                        ${FLOWIE_CONTROL_ACL_GRAMMAR_GEN}
                                        ${FLOWIE_CONTROL_DATA_SCHEMA_SOURCE}
                                        ${FLOWIE_CONTROL_STORE_SQLITE_SOURCE}
                                        ${FLOWIE_CONTROL_STORE_POSTGRESQL_SOURCE}
                                        flowie_control_data_schema_internal.h
                                        flowie_control_data_transfer.c
                                        flowie_control_data_transfer_internal.h
                                        flowie_control_acl.c
                                        flowie_control_acl_internal.h
                                        flowie_control_auth_cache.c
                                        flowie_control_auth_cache_internal.h
                                        flowie_control_bootstrap.c
                                        flowie_control_bootstrap_internal.h
                                        flowie_control_auth_rate_limiter.c
                                        flowie_control_auth_rate_limiter_internal.h
                                        flowie_control_principal_cache.c
                                        flowie_control_principal_cache_internal.h
                                        flowie_control_repository.c
                                        flowie_control_repository_internal.h
                                        flowie_control_external_authenticator.c
                                        flowie_control_external_authenticator_internal.h
                                        flowie_control_auth_service.c
                                        flowie_control_auth_service_internal.h
                                        flowie_control_service_credential.c
                                        flowie_control_service_credential_internal.h
                                        flowie_control_credential.c
                                        flowie_control_credential_internal.h
                                        flowie_control_management_service.c
                                        flowie_control_management_service_internal.h
                                        flowie_control_management_session.c
                                        flowie_control_management_session_internal.h
                                        flowie_control_validation.c
                                        flowie_control_validation_internal.h
                                        flowie_control_database.c
                                        flowie_control_database_internal.h
                                        flowie_control_store_schema.c
                                        flowie_control_store_schema_internal.h
                                        flowie_control_store.c
                                        flowie_control_store_internal.h)
add_dependencies(flowie_control_core ${FLOWIE_CONTROL_ACL_LEXER_TARGET}
                                     ${FLOWIE_CONTROL_ACL_GRAMMAR_TARGET})
cmake_config_target(flowie_control_core NO_INSTALL FOLDER "flowie/control/internal")
target_include_directories(flowie_control_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                               PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
                                                       ${FLOWIE_CONTROL_DATA_GENERATED_DIR}
                                                       ${FLOWIE_CONTROL_STORE_GENERATED_DIR})
target_link_libraries(flowie_control_core
                      PUBLIC Flowie::App flowie_orm_flow
                      PRIVATE Flowie::Protocol
                               TurboUtils::Core TurboUtils::STL monocypher)
add_dependencies(flowie_control_core flowie_control_data_ddl flowie_control_store_ddl)

install(FILES "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
              "${FLOWIE_CONTROL_DATA_POSTGRESQL_DDL}"
              "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
              "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}"
        DESTINATION share/turboflow/control)

  find_package(OpenSSL REQUIRED)
  add_library(
    flowie_control_external_https_authenticator STATIC
    flowie_control_external_https_authenticator.c
    flowie_control_external_https_authenticator_internal.h)
  cmake_config_target(flowie_control_external_https_authenticator NO_INSTALL
                      FOLDER "flowie/control/internal")
  target_include_directories(flowie_control_external_https_authenticator
                             PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(
    flowie_control_external_https_authenticator
    PUBLIC flowie_control_core
    PRIVATE TurboHttp::HttpClient TurboNet::CoroNet TurboUtils::Core
            TurboParser::Parser OpenSSL::SSL OpenSSL::Crypto monocypher)

  add_library(
    flowie_control_jwt_jwks_authenticator STATIC
    flowie_control_jwt_jwks_authenticator.c
    flowie_control_jwt_jwks_authenticator_internal.h)
  cmake_config_target(flowie_control_jwt_jwks_authenticator NO_INSTALL
                      FOLDER "flowie/control/internal")
  target_include_directories(flowie_control_jwt_jwks_authenticator
                             PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                    ${PROJECT_SOURCE_DIR}/flowie/include)
  target_link_libraries(
    flowie_control_jwt_jwks_authenticator
    PRIVATE TurboHttp::Cjwt TurboHttp::HttpClient TurboNet::CoroNet TurboUtils::Core
            TurboParser::Parser OpenSSL::SSL OpenSSL::Crypto)

add_library(flowie_control_startup STATIC flowie_control_config.c
                                          flowie_control_config_internal.h
                                          flowie_control_database_config.c
                                          flowie_control_database_config_internal.h
                                          flowie_control_data_options.c
                                          flowie_control_data_options_internal.h
                                          flowie_control_security_limits_internal.h
                                          flowie_control_startup_options.c
                                          flowie_control_startup_options_internal.h)
cmake_config_target(flowie_control_startup NO_INSTALL FOLDER "flowie/control/internal")
target_include_directories(flowie_control_startup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flowie_control_startup
                      PUBLIC Flowie::App
                      PRIVATE TurboUtils::Core TurboParser::Parser)

  set(FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/dashboard-resources"
      CACHE PATH "Runtime directory for generated Flowie Control dashboard resources")
  file(MAKE_DIRECTORY
       "${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}/assets"
       "${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}/templates")
  foreach(resource IN ITEMS control.css control.js htmx-2.0.9.min.js HTMX_LICENSE.txt
                            HTMX_SOURCE.md)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/${resource}"
                   "${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}/assets/${resource}"
                   COPYONLY)
  endforeach()
  foreach(resource IN ITEMS dashboard.mustache dashboard_content.mustache
                            dashboard_error.mustache login.mustache password.mustache)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/templates/${resource}"
                   "${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}/templates/${resource}"
                   COPYONLY)
  endforeach()

  add_library(flowie_control_iris_adapter STATIC flowie_control_auth_iris_adapter.c
                                                 flowie_control_auth_iris_adapter_internal.h
                                                 flowie_control_auth_iris_endpoint.c
                                                 flowie_control_auth_iris_endpoint_internal.h
                                                 flowie_control_acl_iris_endpoint.c
                                                 flowie_control_acl_iris_endpoint_internal.h
                                                 flowie_control_http_request.c
                                                 flowie_control_http_request_internal.h
                                                 flowie_control_dashboard.c
                                                 flowie_control_dashboard_internal.h
                                                 flowie_control_dashboard_view.c
                                                 flowie_control_dashboard_view_internal.h)
    target_sources(flowie_control_iris_adapter
                   PRIVATE flowie_control_management_rpc.c
                           flowie_control_management_rpc_internal.h)
  cmake_config_target(flowie_control_iris_adapter NO_INSTALL
                      FOLDER "flowie/control/internal")
  target_include_directories(flowie_control_iris_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(
    flowie_control_iris_adapter
    PRIVATE FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR="${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}")
  target_link_libraries(flowie_control_iris_adapter
                        PUBLIC flowie_control_core TurboHttp::Iris
                        PRIVATE flowie Flowie::Protocol TurboHttp::HttpClient TurboNet::CoroNet TurboUtils::Core
                                TurboParser::Mustache TurboParser::Parser monocypher)

    add_library(flowie_control_runtime STATIC flowie_control_runtime.c
                                              flowie_control_runtime_internal.h)
    cmake_config_target(flowie_control_runtime NO_INSTALL
                        FOLDER "flowie/control/internal")
    target_include_directories(flowie_control_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(flowie_control_runtime
                          PUBLIC flowie_control_startup flowie_control_core
                          PRIVATE flowie_control_iris_adapter TurboNet::CoroNet TurboUtils::Core
                                  monocypher)
      target_link_libraries(flowie_control_runtime
                            PRIVATE flowie_control_external_https_authenticator)
      target_compile_definitions(flowie_control_runtime
                                 PRIVATE FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH=1)
      target_link_libraries(flowie_control_runtime
                            PRIVATE flowie_control_jwt_jwks_authenticator)
      target_compile_definitions(flowie_control_runtime
                                 PRIVATE FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH=1)
    add_executable(flowie-control flowie_control_main.c)
    cmake_config_target(flowie-control FOLDER "flowie/control")
    target_link_libraries(flowie-control PRIVATE flowie_control_runtime)
    cmake_target_enable_boringssl_linkage(flowie-control)
    install(TARGETS flowie-control RUNTIME DESTINATION bin)
    add_executable(flowie-control-data flowie_control_data_main.c)
    cmake_config_target(flowie-control-data FOLDER "flowie/control")
    target_link_libraries(flowie-control-data PRIVATE flowie_control_startup flowie_control_core)
    install(TARGETS flowie-control-data RUNTIME DESTINATION bin)
    install(FILES "${PROJECT_SOURCE_DIR}/flowie/examples/flowie-control.yml"
            DESTINATION share/turboflow/examples/flowie OPTIONAL)

    if(BUILD_TESTS)
      find_package(OpenSSL REQUIRED)
      cmake_add_test(
        test_flowie_control_runtime
        SOURCES tests/test_flowie_control_runtime.c
        LIBS flowie_control_runtime TurboUtils::TinyTest
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/tests
        FOLDER "flowie/control/tests")
      cmake_target_enable_boringssl_linkage(test_flowie_control_runtime)
      set_tests_properties(test_flowie_control_runtime
                           PROPERTIES LABELS "flowie;control-plane;security-contract")
      cmake_add_test(
        test_flowie_control_bootstrap_runtime
        SOURCES tests/test_flowie_control_bootstrap_runtime.c
        LIBS flowie_control_runtime TurboUtils::TinyTest
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/tests
        FOLDER "flowie/control/tests")
      cmake_target_enable_boringssl_linkage(test_flowie_control_bootstrap_runtime)
      set_tests_properties(test_flowie_control_bootstrap_runtime
                           PROPERTIES LABELS "flowie;control-plane;security-contract")
      cmake_add_test(
        test_flowie_control_https_integration
        SOURCES tests/test_flowie_control_https_integration.c
        LIBS flowie_control_runtime flowie_control_core TurboHttp::HttpClient
             TurboNet::CoroNet TurboUtils::Core TurboUtils::TinyTest OpenSSL::Crypto
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
        FOLDER "flowie/control/tests")
      cmake_target_enable_boringssl_linkage(test_flowie_control_https_integration)
      target_compile_definitions(
        test_flowie_control_https_integration
        PRIVATE FLOWIE_CONTROL_EXECUTABLE="$<TARGET_FILE:flowie-control>")
      add_dependencies(test_flowie_control_https_integration flowie-control)
      set_tests_properties(
        test_flowie_control_https_integration
        PROPERTIES LABELS "flowie;control-plane;security-contract;integration"
                   TIMEOUT 30)
      if(WIN32)
        target_link_libraries(test_flowie_control_runtime PRIVATE ws2_32)
        target_link_libraries(test_flowie_control_https_integration PRIVATE ws2_32)
        set_target_properties(
          test_flowie_control_runtime
          PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                     "${CMAKE_BINARY_DIR}/flowie/control/test-bin")
        set_target_properties(
          test_flowie_control_bootstrap_runtime
          PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                     "${CMAKE_BINARY_DIR}/flowie/control/test-bin")
        set_target_properties(
          test_flowie_control_https_integration
          PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                     "${CMAKE_BINARY_DIR}/flowie/control/test-bin")
        set_tests_properties(
          test_flowie_control_runtime
          PROPERTIES ENVIRONMENT_MODIFICATION
                     "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::Iris>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::HttpClient>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
        set_tests_properties(
          test_flowie_control_bootstrap_runtime
          PROPERTIES ENVIRONMENT_MODIFICATION
                     "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::Iris>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::HttpClient>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
        set_tests_properties(
          test_flowie_control_https_integration
          PROPERTIES ENVIRONMENT_MODIFICATION
                     "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::Iris>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::HttpClient>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
      endif()
    endif()

if(BUILD_TESTS)
  cmake_add_test(
    test_flowie_control_acl
    SOURCES tests/test_flowie_control_acl.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_acl
                       PROPERTIES LABELS "flowie;control-plane;security-contract;parser")
  cmake_add_test(
    test_flowie_control_bootstrap
    SOURCES tests/test_flowie_control_bootstrap.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_bootstrap
                       PROPERTIES LABELS "flowie;control-plane;security-contract")

  cmake_add_test(
    test_flowie_control_startup_options
    SOURCES tests/test_flowie_control_startup_options.c
    LIBS flowie_control_startup TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_startup_options
                       PROPERTIES LABELS "flowie;control-plane;security-contract")
  cmake_add_test(
    test_flowie_control_config
    SOURCES tests/test_flowie_control_config.c
    LIBS flowie_control_startup TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  target_compile_definitions(
    test_flowie_control_config
    PRIVATE FLOWIE_CONTROL_TEST_CONFIG_PATH="${PROJECT_SOURCE_DIR}/flowie/examples/flowie-control.yml")
  set_tests_properties(test_flowie_control_config
                       PROPERTIES LABELS "flowie;control-plane;security-contract")
  cmake_add_test(
    test_flowie_control_database
    SOURCES tests/test_flowie_control_database.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_database
                       PROPERTIES LABELS "flowie;control-plane;storage-contract")
  cmake_add_test(
    test_flowie_control_data_schema
    SOURCES tests/test_flowie_control_data_schema.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_data_schema
                       PROPERTIES LABELS "flowie;control-plane;storage-contract;data-transfer")
  cmake_add_test(
    test_flowie_control_store_schema
    SOURCES tests/test_flowie_control_store_schema.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_store_schema
                       PROPERTIES LABELS "flowie;control-plane;storage-contract")

  cmake_add_test(
    test_flowie_control_data_transfer
    SOURCES tests/test_flowie_control_data_transfer.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_data_transfer
                       PROPERTIES LABELS "flowie;control-plane;storage-contract;data-transfer")
  cmake_add_test(
    test_flowie_control_data_options
    SOURCES tests/test_flowie_control_data_options.c
    LIBS flowie_control_startup TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_data_options
                       PROPERTIES LABELS "flowie;control-plane;data-transfer")
  cmake_add_test(
    test_flowie_control_store
    SOURCES tests/test_flowie_control_store.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_store
                       PROPERTIES LABELS "flowie;control-plane;security-contract")
  if(FLOWIE_TURBODB_LIVE_TESTS)
    cmake_add_test(
      test_flowie_control_turbodb_live
      SOURCES tests/test_flowie_control_turbodb_live.c
      LIBS flowie_control_core TurboUtils::TinyTest
      INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
      FOLDER "flowie/control/tests")
    set_tests_properties(
      test_flowie_control_turbodb_live
      PROPERTIES LABELS "flowie;control-plane;live;turbodb;postgresql;storage-contract"
                 TIMEOUT 120)
  endif()
  cmake_add_test(
    test_flowie_control_repository
    SOURCES tests/test_flowie_control_repository.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_repository
                       PROPERTIES LABELS "flowie;control-plane;security-contract;storage-contract")
  cmake_add_test(
    test_flowie_control_auth_cache
    SOURCES tests/test_flowie_control_auth_cache.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_auth_cache
                       PROPERTIES LABELS
                                  "flowie;control-plane;security-contract;mqtt-security")
  cmake_add_test(
    test_flowie_control_external_authenticator
    SOURCES tests/test_flowie_control_external_authenticator.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_external_authenticator
                       PROPERTIES LABELS
                                  "flowie;control-plane;security-contract;external-auth")
    cmake_add_test(
      test_flowie_control_external_https_authenticator
      SOURCES tests/test_flowie_control_external_https_authenticator.c
      LIBS flowie_control_external_https_authenticator TurboNet::CoroNet
           TurboUtils::TinyTest
      INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/tests
      FOLDER "flowie/control/tests")
    find_package(OpenSSL REQUIRED)
    target_link_libraries(test_flowie_control_external_https_authenticator
                          PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    cmake_target_enable_boringssl_linkage(test_flowie_control_external_https_authenticator)
    if(WIN32)
      target_link_libraries(test_flowie_control_external_https_authenticator PRIVATE ws2_32)
    endif()
    set_tests_properties(
      test_flowie_control_external_https_authenticator
      PROPERTIES LABELS
                 "flowie;control-plane;security-contract;external-auth;https")
    cmake_add_test(
      test_flowie_control_jwt_jwks_authenticator
      SOURCES tests/test_flowie_control_jwt_jwks_authenticator.c
      LIBS flowie_control_jwt_jwks_authenticator TurboHttp::Cjwt TurboNet::CoroNet
           TurboUtils::TinyTest
      INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/tests
      FOLDER "flowie/control/tests")
    target_link_libraries(test_flowie_control_jwt_jwks_authenticator
                          PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    cmake_target_enable_boringssl_linkage(test_flowie_control_jwt_jwks_authenticator)
    if(WIN32)
      target_link_libraries(test_flowie_control_jwt_jwks_authenticator PRIVATE ws2_32)
    endif()
    set_tests_properties(
      test_flowie_control_jwt_jwks_authenticator
      PROPERTIES LABELS
                 "flowie;control-plane;security-contract;external-auth;jwt;jwks")
    if(WIN32)
      set_target_properties(
        test_flowie_control_external_https_authenticator
        test_flowie_control_jwt_jwks_authenticator
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                   "${CMAKE_BINARY_DIR}/flowie/control/test-bin")
      set_tests_properties(
        test_flowie_control_external_https_authenticator
        PROPERTIES ENVIRONMENT_MODIFICATION
                   "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::HttpClient>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
      set_tests_properties(
        test_flowie_control_jwt_jwks_authenticator
        PROPERTIES ENVIRONMENT_MODIFICATION
                   "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::Cjwt>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::HttpClient>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboParser::Parser>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
  cmake_add_test(
    test_flowie_control_principal_cache
    SOURCES tests/test_flowie_control_principal_cache.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_principal_cache
                       PROPERTIES LABELS "flowie;control-plane;security-contract")
  cmake_add_test(
    test_flowie_control_auth_rate_limiter
    SOURCES tests/test_flowie_control_auth_rate_limiter.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_auth_rate_limiter
                       PROPERTIES LABELS "flowie;control-plane;security-contract")
  cmake_add_test(
    test_flowie_control_auth_service
    SOURCES tests/test_flowie_control_auth_service.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_auth_service
                       PROPERTIES LABELS
                                  "flowie;control-plane;security-contract;mqtt-security")
  cmake_add_test(
    test_flowie_control_management_session
    SOURCES tests/test_flowie_control_management_session.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_management_session
                       PROPERTIES LABELS "flowie;control-plane;security-contract")
  cmake_add_test(
    test_flowie_control_service_credential
    SOURCES tests/test_flowie_control_service_credential.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_service_credential
                       PROPERTIES LABELS
                                  "flowie;control-plane;security-contract;mqtt-security")
  cmake_add_test(
    test_flowie_control_management_service
    SOURCES tests/test_flowie_control_management_service.c
    LIBS flowie_control_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    FOLDER "flowie/control/tests")
  set_tests_properties(test_flowie_control_management_service
                       PROPERTIES LABELS "flowie;control-plane;security-contract")
    cmake_add_test(
      test_flowie_control_auth_iris_adapter
      SOURCES tests/test_flowie_control_auth_iris_adapter.c
      LIBS flowie_control_iris_adapter TurboUtils::TinyTest
      INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
      FOLDER "flowie/control/tests")
    set_tests_properties(test_flowie_control_auth_iris_adapter
                         PROPERTIES LABELS "flowie;control-plane;security-contract")
      cmake_add_test(
        test_flowie_control_management_rpc
        SOURCES tests/test_flowie_control_management_rpc.c
        LIBS flowie_control_iris_adapter TurboUtils::TinyTest
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
        FOLDER "flowie/control/tests")
      set_tests_properties(test_flowie_control_management_rpc
                           PROPERTIES LABELS "flowie;control-plane;security-contract")
    cmake_add_test(
      test_flowie_control_dashboard
      SOURCES tests/test_flowie_control_dashboard.c
      LIBS flowie_control_iris_adapter TurboUtils::TinyTest
      INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
      FOLDER "flowie/control/tests")
    set_tests_properties(test_flowie_control_dashboard
                         PROPERTIES LABELS "flowie;control-plane;security-contract")
    if(WIN32)
      set_target_properties(
        test_flowie_control_auth_iris_adapter
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                   "${CMAKE_BINARY_DIR}/flowie/control/test-bin")
      set_tests_properties(
        test_flowie_control_auth_iris_adapter
        PROPERTIES ENVIRONMENT_MODIFICATION
                   "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::Iris>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
        set_target_properties(
          test_flowie_control_management_rpc
          PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                     "${CMAKE_BINARY_DIR}/flowie/control/test-bin")
        set_tests_properties(
          test_flowie_control_management_rpc
          PROPERTIES ENVIRONMENT_MODIFICATION
                     "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::Iris>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
      set_target_properties(
        test_flowie_control_dashboard
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                   "${CMAKE_BINARY_DIR}/flowie/control/test-bin")
      set_tests_properties(
        test_flowie_control_dashboard
        PROPERTIES ENVIRONMENT_MODIFICATION
                   "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::Iris>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboHttp::HttpClient>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
endif()
