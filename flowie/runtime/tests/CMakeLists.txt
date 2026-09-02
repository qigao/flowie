cmake_add_test(
  test_flow_coronet_runtime
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flow_coronet_runtime.c
  LIBS Flowie::Transport Rocida::TinyTest
  FOLDER "flowie/runtime/tests")
if(WIN32)
  target_link_libraries(test_flow_coronet_runtime PRIVATE ws2_32)
endif()

set_tests_properties(
  test_flow_coronet_runtime
  PROPERTIES LABELS "flowie;runtime;transport;udp;pipe;shutdown")
