add_subdirectory(protocol)
add_subdirectory(client)

set(TARGET_NAME flowie)
set(FLOWIE_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie.h)

add_library(
  ${TARGET_NAME} SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_bitmap_index.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_cluster.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_cluster_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_endpoint.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_ingress.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_ingress_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_message.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_protocol_contract.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_proxy_protocol.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_proxy_protocol_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_protocol_store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_protocol_store_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_security.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_security_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_session.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_session_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_task_group_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_topic_index.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_topic_index_internal.h
  ${FLOWIE_HEADERS})
cmake_config_target(
  ${TARGET_NAME}
  ALIAS Flowie::Flowie
  FOLDER "flowie"
  EXPORT_NAME Flowie)
add_library(Flowie::App ALIAS ${TARGET_NAME})
target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Flowie::Protocol TurboNet::CoroNet TurboUtils::Core
  PRIVATE roaring::roaring xxHash::xxhash)
if(WIN32)
  target_link_libraries(${TARGET_NAME} PRIVATE ws2_32)
endif()
target_link_libraries(${TARGET_NAME} PRIVATE Flowie::Client Flowie::Transport)
install(FILES ${FLOWIE_HEADERS}
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_message.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_execution.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_protocol_contract.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_record_store.h
              ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_security.h
        DESTINATION include)

if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
