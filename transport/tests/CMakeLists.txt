cmake_add_test(
  test_flow_execution
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flow_execution.c
  LIBS Flowie::Transport Salts::TinyTest
  FOLDER "transport/tests")

set_tests_properties(
  test_flow_execution
  PROPERTIES LABELS "flowie;transport;execution;coroutine")
