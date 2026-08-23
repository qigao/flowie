cmake_add_test(
  test_flowie_core
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_core.c
  LIBS Flowie::App TurboUtils::TinyTest
  FOLDER "flowie/tests")

set_tests_properties(test_flowie_core PROPERTIES LABELS "flowie;flowie-release;mqtt-fast")

cmake_add_test(
  test_flowie_protocol_repository
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_protocol_repository.c
  LIBS Flowie::App TurboUtils::TinyTest
  FOLDER "flowie/tests")

set_tests_properties(test_flowie_protocol_repository PROPERTIES LABELS "flowie;flowie-release;storage")

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
