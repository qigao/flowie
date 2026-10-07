add_library(
  flowie_connection STATIC
  src/flowie_connection.c
  src/flowie_send_batch.c
  src/flowie_send_batch.h
  src/flowie_affinity.c
  src/flowie_affinity.h
  include/flowie_connection.h)

cmake_config_target(
  flowie_connection
  ALIAS Flowie::Connection
  NO_INSTALL
  FOLDER "connection")

set_target_properties(flowie_connection PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  flowie_connection
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(
  flowie_connection
  PUBLIC Salts::Core Salts::CNet CHttp::Server)

if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(FLOWIE_BUILD_BENCHMARKS)
  cmake_add_benchmark(
    benchmark_flowie_send_batch
    SOURCES tests/benchmark_flowie_send_batch.c
    LIBS Flowie::Connection Salts::TinyTest
    FOLDER "connection/benchmarks")
  add_test(NAME benchmark_flowie_send_batch COMMAND benchmark_flowie_send_batch)
  set_tests_properties(benchmark_flowie_send_batch PROPERTIES LABELS "benchmark;flowie;connection")
endif()
