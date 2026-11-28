set(FLOWIE_SERVER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(FLOWIE_SERVER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")

add_executable(
  flowie_server ${FLOWIE_SERVER_SOURCE_DIR}/flowie_server.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_rulesforge_package.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_rulesforge_pipeline.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_rulesforge_worker.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_server_config.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_server_http_security.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_server_release.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_server_runtime.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_server_turbodb_config.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_workflow_host.c
  ${FLOWIE_SERVER_SOURCE_DIR}/flowie_workflow_https_sink.c)
target_link_libraries(
  flowie_server
  PRIVATE
    Flowie::App
    flowie_control_release_http
    Orm::C
    CHttp::Client
    Salts::CmdParser
    Salts::Core
    Salts::Crypto
    Salts::CYaml
    Salts::JsonParser
    Salts::URI
    CMeta::ACE
    CMeta::CMeta)
install(TARGETS flowie_server RUNTIME DESTINATION bin)

if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
