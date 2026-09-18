cmake_add_test(
  test_flowie_connection
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_connection.c
  LIBS Flowie::Connection CHttp::Client CHttp::Server Salts::CNet Salts::TinyTest
  FOLDER "connection/tests")

if(WIN32)
  target_link_libraries(test_flowie_connection PRIVATE ws2_32)
  set_tests_properties(
    test_flowie_connection
    PROPERTIES ENVIRONMENT_MODIFICATION
               "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:CHttp::Client>;PATH=path_list_prepend:$<TARGET_FILE_DIR:CHttp::Server>")
endif()
set_tests_properties(
  test_flowie_connection
  PROPERTIES LABELS "flowie;connection;cnet;chttp;tcp;udp;kcp;websocket")
