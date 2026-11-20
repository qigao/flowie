set(FLOWIE_CONTROL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

cmake_add_grammar(
  FLOWIE_CONTROL_ACL
  LEXER_RE
  ${CMAKE_CURRENT_SOURCE_DIR}/parser/acl_lexer.re
  GRAMMAR_Y
  ${CMAKE_CURRENT_SOURCE_DIR}/parser/acl_grammar.y
  LEXER_DEPENDS_ON_GRAMMAR
  FOLDER
  "flowie/control/acl")

set(_idlc_root "$ENV{SALTS_UTILS_ROOT}")
if(CMAKE_CROSSCOMPILING)
  # The generator and its runtime libraries must match the build host.
  if("$ENV{SALTS_IDLC_ROOT}" STREQUAL "" OR "$ENV{SALTS_IDLC_BASE_ROOT}" STREQUAL "")
    message(FATAL_ERROR "Cross builds require host SALTS_IDLC_ROOT and SALTS_IDLC_BASE_ROOT")
  endif()
  set(_idlc_root "$ENV{SALTS_IDLC_ROOT}")
  if(CMAKE_HOST_WIN32)
    set(FLOWIE_CONTROL_TBE_RUNTIME_ENV
        "PATH=$ENV{SALTS_IDLC_ROOT}/bin\;$ENV{SALTS_IDLC_BASE_ROOT}/bin\;$ENV{PATH}")
  elseif(CMAKE_HOST_APPLE)
    set(FLOWIE_CONTROL_TBE_RUNTIME_ENV
        "DYLD_LIBRARY_PATH=$ENV{SALTS_IDLC_ROOT}/lib:$ENV{SALTS_IDLC_BASE_ROOT}/lib")
  else()
    set(FLOWIE_CONTROL_TBE_RUNTIME_ENV
        "LD_LIBRARY_PATH=$ENV{SALTS_IDLC_ROOT}/lib:$ENV{SALTS_IDLC_BASE_ROOT}/lib")
  endif()
elseif(WIN32)
  set(FLOWIE_CONTROL_TBE_RUNTIME_ENV
      "PATH=$<TARGET_FILE_DIR:Salts::Mustache>\;$<TARGET_FILE_DIR:Salts::Core>\;$ENV{PATH}"
  )
elseif(APPLE)
  set(FLOWIE_CONTROL_TBE_RUNTIME_ENV
      "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Salts::Mustache>:$<TARGET_FILE_DIR:Salts::Core>:$ENV{DYLD_LIBRARY_PATH}"
  )
else()
  set(FLOWIE_CONTROL_TBE_RUNTIME_ENV
      "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Salts::Mustache>:$<TARGET_FILE_DIR:Salts::Core>:$ENV{LD_LIBRARY_PATH}"
  )
endif()
unset(FLOWIE_CONTROL_SALTS_IDLC CACHE)
find_program(
  FLOWIE_CONTROL_SALTS_IDLC
  NAMES salts-idlc salts-idlc.exe
  PATHS "${_idlc_root}/bin"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
set(FLOWIE_CONTROL_DATA_SCHEMA
    "${CMAKE_CURRENT_SOURCE_DIR}/schema/flowie_control_data.schema")
set(FLOWIE_CONTROL_DATA_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/data-schema")
set(FLOWIE_CONTROL_DATA_SQLITE_DDL
    "${FLOWIE_CONTROL_DATA_GENERATED_DIR}/flowie-control-data.sqlite.sql")
set(FLOWIE_CONTROL_DATA_POSTGRESQL_DDL
    "${FLOWIE_CONTROL_DATA_GENERATED_DIR}/flowie-control-data.postgresql.sql")
set(FLOWIE_CONTROL_DATA_SCHEMA_SOURCE
    "${FLOWIE_CONTROL_DATA_GENERATED_DIR}/flowie_control_data_schema.c")
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${FLOWIE_CONTROL_DATA_GENERATED_DIR}"
  COMMAND
    "${CMAKE_COMMAND}" -E env "${FLOWIE_CONTROL_TBE_RUNTIME_ENV}"
    "${FLOWIE_CONTROL_SALTS_IDLC}" "${FLOWIE_CONTROL_DATA_SCHEMA}" --lang sqlite
    --output "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
  DEPENDS "${FLOWIE_CONTROL_DATA_SCHEMA}" "${FLOWIE_CONTROL_SALTS_IDLC}"
  COMMENT "Generating Flowie Control SQLite data-manifest schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_DATA_POSTGRESQL_DDL}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${FLOWIE_CONTROL_DATA_GENERATED_DIR}"
  COMMAND
    "${CMAKE_COMMAND}" -E env "${FLOWIE_CONTROL_TBE_RUNTIME_ENV}"
    "${FLOWIE_CONTROL_SALTS_IDLC}" "${FLOWIE_CONTROL_DATA_SCHEMA}" --lang
    postgresql --output "${FLOWIE_CONTROL_DATA_POSTGRESQL_DDL}"
  DEPENDS "${FLOWIE_CONTROL_DATA_SCHEMA}" "${FLOWIE_CONTROL_SALTS_IDLC}"
  COMMENT "Generating Flowie Control PostgreSQL data-manifest schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_DATA_SCHEMA_SOURCE}"
  COMMAND
    "${CMAKE_COMMAND}" -DINPUT=${FLOWIE_CONTROL_DATA_SQLITE_DDL}
    -DOUTPUT=${FLOWIE_CONTROL_DATA_SCHEMA_SOURCE}
    -DSYMBOL=FLOWIE_CONTROL_DATA_SCHEMA_BYTES
    -DHEADER=flowie_control_data_schema_internal.h
    -DFUNCTION=flowie_control_data_schema_sql -P
    "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  DEPENDS "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
          "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  COMMENT "Embedding Flowie Control data-manifest schema"
  VERBATIM)
add_custom_target(
  flowie_control_data_ddl DEPENDS "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
                                  "${FLOWIE_CONTROL_DATA_POSTGRESQL_DDL}")
set_target_properties(flowie_control_data_ddl PROPERTIES FOLDER
                                                         "flowie/control/data")

set(FLOWIE_CONTROL_STORE_SCHEMA
    "${CMAKE_CURRENT_SOURCE_DIR}/schema/flowie_control_store.schema")
set(FLOWIE_CONTROL_STORE_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/store-schema")
set(FLOWIE_CONTROL_STORE_SQLITE_DDL
    "${FLOWIE_CONTROL_STORE_GENERATED_DIR}/flowie-control-store.sqlite.sql")
set(FLOWIE_CONTROL_STORE_POSTGRESQL_DDL
    "${FLOWIE_CONTROL_STORE_GENERATED_DIR}/flowie-control-store.postgresql.sql")
set(FLOWIE_CONTROL_STORE_SQLITE_SOURCE
    "${FLOWIE_CONTROL_STORE_GENERATED_DIR}/flowie_control_store_schema_sqlite.c"
)
set(FLOWIE_CONTROL_STORE_POSTGRESQL_SOURCE
    "${FLOWIE_CONTROL_STORE_GENERATED_DIR}/flowie_control_store_schema_postgresql.c"
)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${FLOWIE_CONTROL_STORE_GENERATED_DIR}"
  COMMAND
    "${CMAKE_COMMAND}" -E env "${FLOWIE_CONTROL_TBE_RUNTIME_ENV}"
    "${FLOWIE_CONTROL_SALTS_IDLC}" "${FLOWIE_CONTROL_STORE_SCHEMA}" --lang
    sqlite --output "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
  DEPENDS "${FLOWIE_CONTROL_STORE_SCHEMA}" "${FLOWIE_CONTROL_SALTS_IDLC}"
  COMMENT "Generating Flowie Control SQLite store schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${FLOWIE_CONTROL_STORE_GENERATED_DIR}"
  COMMAND
    "${CMAKE_COMMAND}" -E env "${FLOWIE_CONTROL_TBE_RUNTIME_ENV}"
    "${FLOWIE_CONTROL_SALTS_IDLC}" "${FLOWIE_CONTROL_STORE_SCHEMA}" --lang
    postgresql --output "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}"
  DEPENDS "${FLOWIE_CONTROL_STORE_SCHEMA}" "${FLOWIE_CONTROL_SALTS_IDLC}"
  COMMENT "Generating Flowie Control PostgreSQL store schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_STORE_SQLITE_SOURCE}"
  COMMAND
    "${CMAKE_COMMAND}" -DINPUT=${FLOWIE_CONTROL_STORE_SQLITE_DDL}
    -DOUTPUT=${FLOWIE_CONTROL_STORE_SQLITE_SOURCE}
    -DSYMBOL=FLOWIE_CONTROL_STORE_SQLITE_SCHEMA_BYTES
    -DHEADER=flowie_control_store_schema_internal.h
    -DFUNCTION=flowie_control_store_schema_sqlite_sql -P
    "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  DEPENDS "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
          "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  COMMENT "Embedding Flowie Control SQLite store schema"
  VERBATIM)
add_custom_command(
  OUTPUT "${FLOWIE_CONTROL_STORE_POSTGRESQL_SOURCE}"
  COMMAND
    "${CMAKE_COMMAND}" -DINPUT=${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}
    -DOUTPUT=${FLOWIE_CONTROL_STORE_POSTGRESQL_SOURCE}
    -DSYMBOL=FLOWIE_CONTROL_STORE_POSTGRESQL_SCHEMA_BYTES
    -DHEADER=flowie_control_store_schema_internal.h
    -DFUNCTION=flowie_control_store_schema_postgresql_sql -P
    "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  DEPENDS "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}"
          "${PROJECT_SOURCE_DIR}/cmake/EmbedGeneratedText.cmake"
  COMMENT "Embedding Flowie Control PostgreSQL store schema"
  VERBATIM)
add_custom_target(
  flowie_control_store_ddl DEPENDS "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
                                   "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}")
set_target_properties(flowie_control_store_ddl PROPERTIES FOLDER
                                                          "flowie/control/data")

add_library(
  flowie_control_core STATIC
  ${FLOWIE_CONTROL_ACL_LEXER_GEN}
  ${FLOWIE_CONTROL_ACL_GRAMMAR_GEN}
  ${FLOWIE_CONTROL_DATA_SCHEMA_SOURCE}
  ${FLOWIE_CONTROL_STORE_SQLITE_SOURCE}
  ${FLOWIE_CONTROL_STORE_POSTGRESQL_SOURCE}
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_data_schema_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_data_transfer.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_data_transfer_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_acl.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_acl_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_cache.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_cache_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_bootstrap.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_bootstrap_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_rate_limiter.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_rate_limiter_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_principal_cache.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_principal_cache_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_repository.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_repository_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_external_authenticator.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_external_authenticator_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_service.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_service_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_service_credential.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_service_credential_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_credential.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_credential_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_management_service.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_management_service_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_management_session.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_management_session_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_validation.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_validation_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_database.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_database_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_store_schema.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_store_schema_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_store.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_store_internal.h)
add_dependencies(flowie_control_core ${FLOWIE_CONTROL_ACL_LEXER_TARGET}
                 ${FLOWIE_CONTROL_ACL_GRAMMAR_TARGET})
cmake_config_target(flowie_control_core NO_INSTALL FOLDER
                    "flowie/control/internal")
target_include_directories(
  flowie_control_core
  PUBLIC ${FLOWIE_CONTROL_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/core/include
         ${PROJECT_SOURCE_DIR}/flowie/transport/include
  PRIVATE ${FLOWIE_PRIVATE_INCLUDE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
          ${FLOWIE_CONTROL_DATA_GENERATED_DIR}
          ${FLOWIE_CONTROL_STORE_GENERATED_DIR})
target_link_libraries(
  flowie_control_core
  PRIVATE Salts::Core CMeta::CSTL monocypher $<LINK_ONLY:flowie>
  PUBLIC flowie_orm_flow Flowie::Protocol)
add_dependencies(flowie_control_core flowie_control_data_ddl
                 flowie_control_store_ddl)

install(
  FILES "${FLOWIE_CONTROL_DATA_SQLITE_DDL}"
        "${FLOWIE_CONTROL_DATA_POSTGRESQL_DDL}"
        "${FLOWIE_CONTROL_STORE_SQLITE_DDL}"
        "${FLOWIE_CONTROL_STORE_POSTGRESQL_DDL}"
  DESTINATION share/turboflow/flowie/control)

add_library(
  flowie_control_external_https_authenticator STATIC
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_external_https_authenticator.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_external_https_authenticator_internal.h
)
cmake_config_target(flowie_control_external_https_authenticator NO_INSTALL
                    FOLDER "flowie/control/internal")
target_include_directories(flowie_control_external_https_authenticator
                           PUBLIC ${FLOWIE_CONTROL_SOURCE_DIR})
target_link_libraries(
  flowie_control_external_https_authenticator
  PUBLIC flowie_control_core
  PRIVATE CHttp::Client Salts::Core Salts::JsonParser Salts::URI monocypher)

add_library(
  flowie_control_jwt_jwks_authenticator STATIC
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_jwt_jwks_authenticator.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_jwt_jwks_authenticator_internal.h)
cmake_config_target(flowie_control_jwt_jwks_authenticator NO_INSTALL FOLDER
                    "flowie/control/internal")
target_include_directories(
  flowie_control_jwt_jwks_authenticator
  PUBLIC ${FLOWIE_CONTROL_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/core/include
         ${PROJECT_SOURCE_DIR}/flowie/transport/include)
target_link_libraries(
  flowie_control_jwt_jwks_authenticator
  PRIVATE CHttp::JWT CHttp::Client Salts::Core Salts::JsonParser Salts::URI)

add_library(
  flowie_control_startup STATIC
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_config.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_config_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_database_config.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_database_config_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_data_options.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_data_options_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_security_limits_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_startup_options.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_startup_options_internal.h)
cmake_config_target(flowie_control_startup NO_INSTALL FOLDER
                    "flowie/control/internal")
target_include_directories(
  flowie_control_startup
  PUBLIC ${FLOWIE_CONTROL_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/flowie/core/include
         ${PROJECT_SOURCE_DIR}/flowie/transport/include)
target_link_libraries(
  flowie_control_startup
  PRIVATE Orm::C
          Salts::Core
          CMeta::CFlow
          Salts::CmdParser
          Salts::CYaml
          Salts::CYamlJsonAdapter
          Salts::JsonParser)

set(FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/dashboard-resources"
    CACHE PATH
          "Runtime directory for generated Flowie Control dashboard resources")
file(MAKE_DIRECTORY "${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}/assets"
     "${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}/templates")
foreach(resource IN ITEMS control.css control.js htmx-2.0.9.min.js
                          HTMX_LICENSE.txt HTMX_SOURCE.md)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/${resource}"
    "${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}/assets/${resource}" COPYONLY)
endforeach()
foreach(
  resource IN
  ITEMS dashboard.html
        dashboard_content.html
        dashboard_overview.html
        dashboard_integration.html
        dashboard_users.html
        dashboard_groups.html
        dashboard_roles.html
        dashboard_acls.html
        dashboard_audit.html
        dashboard_error.html
        login.html
        password.html)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/templates/${resource}"
    "${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}/templates/${resource}" COPYONLY)
endforeach()

add_library(
  flowie_control_http STATIC
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_http_request.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_http_request_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_http_server.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_http_server_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_rpc.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_rpc_internal.h)
cmake_config_target(flowie_control_http NO_INSTALL FOLDER
                    "flowie/control/internal")
target_include_directories(flowie_control_http
                           PUBLIC ${FLOWIE_CONTROL_SOURCE_DIR})
target_link_libraries(
  flowie_control_http
  PUBLIC Salts::Core
  PRIVATE CHttp::App Salts::JsonParser)

add_library(
  flowie_control_iris_adapter STATIC
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_iris_adapter.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_iris_adapter_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_async.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_async_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_iris_endpoint.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_auth_iris_endpoint_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_acl_iris_endpoint.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_acl_iris_endpoint_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_dashboard.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_dashboard_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_dashboard_view.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_dashboard_view_internal.h
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_dashboard_model_internal.h)
target_sources(
  flowie_control_iris_adapter
  PRIVATE ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_management_rpc.c
          ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_management_rpc_internal.h)
cmake_config_target(flowie_control_iris_adapter NO_INSTALL FOLDER
                    "flowie/control/internal")
target_include_directories(
  flowie_control_iris_adapter
  PUBLIC ${FLOWIE_CONTROL_SOURCE_DIR}
  PRIVATE ${PROJECT_SOURCE_DIR}/flowie/core/include
          ${PROJECT_SOURCE_DIR}/flowie/transport/include)
target_compile_definitions(
  flowie_control_iris_adapter
  PRIVATE
    FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR="${FLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR}"
)
target_link_libraries(
  flowie_control_iris_adapter
  PRIVATE flowie_control_http
          CHttp::App
          CMeta::CMeta
          Salts::Core
          Salts::Coroutine
          Salts::JsonParser
          monocypher
          $<LINK_ONLY:flowie>
  PUBLIC flowie_control_core Flowie::Protocol)

add_library(
  flowie_control_runtime STATIC
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_runtime.c
  ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_runtime_internal.h)
cmake_config_target(flowie_control_runtime NO_INSTALL FOLDER
                    "flowie/control/internal")
target_include_directories(flowie_control_runtime
                           PUBLIC ${FLOWIE_CONTROL_SOURCE_DIR})
target_link_libraries(
  flowie_control_runtime
  PUBLIC flowie_control_startup flowie_control_core
  PRIVATE flowie_control_iris_adapter CHttp::Server Salts::Core monocypher)
target_link_libraries(flowie_control_runtime
                      PRIVATE flowie_control_external_https_authenticator)
target_compile_definitions(flowie_control_runtime
                           PRIVATE FLOWIE_CONTROL_HAS_EXTERNAL_HTTPS_AUTH=1)
target_link_libraries(flowie_control_runtime
                      PRIVATE flowie_control_jwt_jwks_authenticator)
target_compile_definitions(flowie_control_runtime
                           PRIVATE FLOWIE_CONTROL_HAS_JWT_JWKS_AUTH=1)
add_executable(flowie-control
               ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_main.c)
cmake_config_target(flowie-control FOLDER "flowie/control")
target_link_libraries(flowie-control PRIVATE flowie_control_runtime)
install(TARGETS flowie-control RUNTIME DESTINATION bin)
add_executable(flowie-control-data
               ${FLOWIE_CONTROL_SOURCE_DIR}/flowie_control_data_main.c)
cmake_config_target(flowie-control-data FOLDER "flowie/control")
target_link_libraries(flowie-control-data PRIVATE flowie_control_startup
                                                  flowie_control_core)
install(TARGETS flowie-control-data RUNTIME DESTINATION bin)
install(
  FILES "${PROJECT_SOURCE_DIR}/flowie/examples/flowie-control.yml"
  DESTINATION share/turboflow/examples/flowie
  OPTIONAL)

if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
