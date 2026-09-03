add_subdirectory(protocol)
add_subdirectory(client)

set(TARGET_NAME flowie)

add_library(flowie_orm_flow STATIC src/flowie_orm_flow.c)
set_target_properties(flowie_orm_flow PROPERTIES POSITION_INDEPENDENT_CODE ON)
cmake_config_target(flowie_orm_flow NO_INSTALL FOLDER "flowie/internal")
target_include_directories(flowie_orm_flow PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(flowie_orm_flow PUBLIC Orm::C PRIVATE Salts::CFlow Salts::Core)
if(TARGET Orm::PostgreSQL)
  target_compile_definitions(flowie_orm_flow PRIVATE FLOWIE_ORM_WITH_POSTGRESQL)
  target_link_libraries(flowie_orm_flow PUBLIC Orm::PostgreSQL)
endif()
set(FLOWIE_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_export.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie.h)

cmake_add_grammar(FLOWIE_SECURITY_RULE
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/security_rule_lexer.re
  FOLDER "flowie/parser")

add_library(
  ${TARGET_NAME} SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_bitmap_index.c
  ${PROJECT_SOURCE_DIR}/cluster/flowie_cluster.c
  ${PROJECT_SOURCE_DIR}/cluster/flowie_cluster_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_endpoint.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_ingress.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_ingress_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_message.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_protocol_contract.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_proxy_protocol.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_proxy_protocol_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_protocol_repository.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_security.c
  ${FLOWIE_SECURITY_RULE_LEXER_GEN}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_security_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_session.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_session_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_task_group_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_topic_index.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_topic_index_internal.h
  ${FLOWIE_HEADERS})
add_dependencies(${TARGET_NAME} ${FLOWIE_SECURITY_RULE_LEXER_TARGET})
cmake_config_target(
  ${TARGET_NAME}
  ALIAS Flowie::Flowie
  FOLDER "flowie"
  EXPORT_NAME Flowie)
add_library(Flowie::App ALIAS ${TARGET_NAME})
target_compile_definitions(${TARGET_NAME} PRIVATE FLOWIE_BUILD)
target_include_directories(${TARGET_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/cluster)
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Flowie::Protocol Orm::C Salts::Core Salts::Coroutine
  PRIVATE Flowie::NetRuntime Salts::CSTL Salts::LtvParser roaring::roaring xxHash::xxhash)
if(WIN32)
  target_link_libraries(${TARGET_NAME} PRIVATE ws2_32)
endif()
target_link_libraries(${TARGET_NAME} PRIVATE Flowie::Client Flowie::Transport flowie_orm_flow)

install(FILES ${FLOWIE_HEADERS}
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_message.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_mqtt_security.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_execution.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_protocol_contract.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_protocol_repository.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_security.h
        DESTINATION include)


if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
