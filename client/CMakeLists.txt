set(TARGET_NAME flowie_client)

set(FLOWIE_CLIENT_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/flowie_mqtt_client.h)

add_library(
  ${TARGET_NAME} SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/src/flowie_mqtt_client.c
  ${FLOWIE_CLIENT_HEADERS})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS Flowie::Client
  FOLDER "client"
  EXPORT_NAME Client)

target_compile_definitions(${TARGET_NAME} PRIVATE FLOWIE_MQTT_CLIENT_BUILD)
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/protocol/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${PROJECT_SOURCE_DIR}/flowie/include)
target_link_libraries(
  ${TARGET_NAME}
  PRIVATE Flowie::Protocol Salts::Core Salts::CSTL Salts::CNet CHttp::Client
          monocypher)

install(FILES ${FLOWIE_CLIENT_HEADERS} DESTINATION include)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(FLOWIE_BUILD_BENCHMARKS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The peer is a separate process so its owner/acceptor CPU is not charged to
  # the measured Client. Linux process clocks and spawn plumbing stay test-only.
  cmake_add_executable(flowie_mqtt_progress_peer
    SOURCES tests/flowie_mqtt_progress_peer.c
    INCLUDES ${PROJECT_SOURCE_DIR}/flowie/tests
    LIBS Flowie::Connection Flowie::Protocol Salts::Core
    FOLDER "client/benchmarks")
  cmake_add_benchmark(benchmark_flowie_mqtt_progress
    SOURCES tests/benchmark_flowie_mqtt_progress.c
    INCLUDES ${PROJECT_SOURCE_DIR}/flowie/tests
    LIBS Flowie::Client Flowie::Protocol Salts::Core Salts::TinyTest
    DEFS FLOWIE_PROGRESS_PEER_PATH="$<TARGET_FILE:flowie_mqtt_progress_peer>"
    FOLDER "client/benchmarks")
  add_dependencies(benchmark_flowie_mqtt_progress flowie_mqtt_progress_peer)
  add_test(NAME benchmark_flowie_mqtt_progress COMMAND benchmark_flowie_mqtt_progress)
  set_tests_properties(benchmark_flowie_mqtt_progress PROPERTIES
    LABELS "benchmark;flowie;mqtt;client;poll-wake" RUN_SERIAL TRUE TIMEOUT 240)
endif()
