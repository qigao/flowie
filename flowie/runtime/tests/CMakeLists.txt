cmake_add_test(
  test_flow_execution
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flow_execution.c
  LIBS Flowie::Transport Salts::TinyTest
  FOLDER "flowie/runtime/tests")

set_tests_properties(
  test_flow_execution
  PROPERTIES LABELS "flowie;runtime;execution;coroutine")

cmake_add_test(
  test_flow_net_runtime
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flow_net_runtime.c
  LIBS Flowie::NetRuntime Salts::TinyTest
  FOLDER "flowie/runtime/tests")
if(WIN32)
  target_link_libraries(test_flow_net_runtime PRIVATE ws2_32)
  set_tests_properties(
    test_flow_net_runtime
    PROPERTIES ENVIRONMENT_MODIFICATION
               "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CHTTP>")
endif()
set_tests_properties(
  test_flow_net_runtime
  PROPERTIES LABELS "flowie;runtime;transport;cnet;chttp;tcp;udp;kcp;websocket")
