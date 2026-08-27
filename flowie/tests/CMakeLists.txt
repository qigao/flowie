cmake_add_test(
  test_flowie_core
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_core.c
  LIBS Flowie::App TurboUtils::TinyTest
  FOLDER "flowie/tests")

set_tests_properties(test_flowie_core PROPERTIES LABELS "flowie;flowie-release;mqtt-fast")

cmake_add_test(
  test_flowie_session_delivery_index
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_session_delivery_index.c
  LIBS Flowie::App TurboUtils::TinyTest
  FOLDER "flowie/tests")
target_include_directories(test_flowie_session_delivery_index
                           PRIVATE ${PROJECT_SOURCE_DIR}/flowie/src)
set_tests_properties(test_flowie_session_delivery_index
                     PROPERTIES LABELS "flowie;flowie-release;mqtt-fast;session")

cmake_add_test(
  test_flowie_transport_baseline
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_transport_baseline.c
  LIBS Flowie::App Flowie::Client TurboUtils::TinyTest
  FOLDER "flowie/tests")
if(WIN32)
  target_link_libraries(test_flowie_transport_baseline PRIVATE ws2_32)
endif()

set_tests_properties(
  test_flowie_transport_baseline
  PROPERTIES LABELS "flowie;flowie-release;mqtt-transport;mqtt-security"
             TIMEOUT 120)

cmake_add_test(
  test_flowie_protocol_repository
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_protocol_repository.c
  LIBS Flowie::App SQLite::SQLite3 TurboUtils::TinyTest
  FOLDER "flowie/tests")

set_tests_properties(test_flowie_protocol_repository PROPERTIES LABELS "flowie;flowie-release;storage")

if(FLOWIE_CONTROL_PGSQL)
  find_package(PostgreSQL REQUIRED)
  cmake_add_test(
    test_flowie_protocol_repository_pgsql_live
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_protocol_repository_pgsql_live.c
    LIBS Flowie::App PostgreSQL::PostgreSQL TurboUtils::TinyTest TurboUtils::Core
    FOLDER "flowie/tests")
  set_tests_properties(
    test_flowie_protocol_repository_pgsql_live
    PROPERTIES LABELS "flowie;flowie-release;storage;postgresql;integration"
               TIMEOUT 300)
  if(NOT TURBO_FLOW_PGSQL_LIVE_TESTS)
    set_tests_properties(test_flowie_protocol_repository_pgsql_live PROPERTIES DISABLED TRUE)
  endif()
  if(WIN32)
    set_tests_properties(
      test_flowie_protocol_repository_pgsql_live
      PROPERTIES ENVIRONMENT_MODIFICATION
                 "PATH=path_list_prepend:$<TARGET_FILE_DIR:PostgreSQL::PostgreSQL>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  endif()
endif()

cmake_add_test(
  test_flowie_cluster
  SOURCES ${PROJECT_SOURCE_DIR}/cluster/tests/test_flowie_cluster.c
  LIBS Flowie::App TurboUtils::TinyTest
  FOLDER "flowie/tests")
target_include_directories(test_flowie_cluster PRIVATE ${PROJECT_SOURCE_DIR}/cluster)
set_tests_properties(test_flowie_cluster PROPERTIES LABELS "flowie;cluster;flowie-release")

cmake_add_test(
  test_flowie_topic_index
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_topic_index.c
  LIBS Flowie::App TurboUtils::STL TurboUtils::TinyTest
  FOLDER "flowie/tests")
target_include_directories(test_flowie_topic_index
                           PRIVATE ${PROJECT_SOURCE_DIR}/cluster
                                   ${PROJECT_SOURCE_DIR}/flowie/src)
set_tests_properties(test_flowie_topic_index PROPERTIES LABELS "flowie;mqtt;topic-index")

cmake_add_test(
  test_flowie_cluster_topology
  SOURCES ${PROJECT_SOURCE_DIR}/cluster/tests/test_flowie_cluster_topology.c
  LIBS flowie_cluster_topology TurboUtils::TinyTest
  FOLDER "flowie/tests")
set_tests_properties(test_flowie_cluster_topology
                     PROPERTIES LABELS "flowie;cluster;topology")

cmake_add_test(
  test_flowie_cluster_publish_event
  SOURCES ${PROJECT_SOURCE_DIR}/cluster/tests/test_flowie_cluster_publish_event.c
  LIBS flowie_cluster_publish_event TurboUtils::TinyTest
  FOLDER "flowie/tests")
set_tests_properties(test_flowie_cluster_publish_event
                     PROPERTIES LABELS "flowie;cluster;publish;event")

cmake_add_test(
  test_flowie_cluster_peer_protocol
  SOURCES ${PROJECT_SOURCE_DIR}/cluster/tests/test_flowie_cluster_peer_protocol.c
  LIBS flowie_cluster_peer TurboUtils::TinyTest
  FOLDER "flowie/tests")
set_tests_properties(test_flowie_cluster_peer_protocol
                     PROPERTIES LABELS "flowie;cluster;peer;protocol")

foreach(peer_test IN ITEMS
        registry
        connect_bind
        connection_lost
        mqtt_command
        publish_settle
        mqtt_reply
        edge_action
        takeover_close)
  cmake_add_test(
  test_flowie_cluster_peer_${peer_test}
    SOURCES ${PROJECT_SOURCE_DIR}/cluster/tests/test_flowie_cluster_peer_${peer_test}.c
    LIBS flowie_cluster_peer TurboUtils::TinyTest
    FOLDER "flowie/tests")
  set_tests_properties(test_flowie_cluster_peer_${peer_test}
                       PROPERTIES LABELS "flowie;cluster;peer;protocol")
endforeach()
