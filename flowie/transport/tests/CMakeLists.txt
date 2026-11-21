cmake_add_test(
  test_flowie_connection
  SOURCES
  test_flowie_connection.c
  LIBS
  Flowie::Connection
  CHttp::Client
  CHttp::Server
  Salts::CNet
  Salts::TinyTest
  FOLDER
  "flowie/transport/tests")
cmake_add_test(
  test_flowie_send_batch
  SOURCES
  test_flowie_send_batch.c
  LIBS
  Flowie::Connection
  Salts::TinyTest
  FOLDER
  "flowie/transport/tests")
set_tests_properties(
  test_flowie_connection test_flowie_send_batch
  PROPERTIES LABELS "flowie;connection;ownership")
if(WIN32)
  target_link_libraries(test_flowie_connection PRIVATE ws2_32)
endif()

cmake_add_test(
  test_flow_execution
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flow_execution.c
  LIBS
  Flowie::Transport
  Salts::TinyTest
  FOLDER
  "flowie/transport/tests")

set_tests_properties(
  test_flow_execution
  PROPERTIES LABELS "flowie;flowie/transport;execution;coroutine")
