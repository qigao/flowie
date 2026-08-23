cmake_add_test(
  test_flowie_cluster_publish_stream
  SOURCES test_flowie_cluster_publish_stream.c
  LIBS flowie_cluster_publish_stream TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_publish_stream
  PROPERTIES LABELS "flowie;cluster;turboraft;flowmq;data-stream;publish")

cmake_add_test(
  test_flowie_cluster_publish_ingress
  SOURCES test_flowie_cluster_publish_ingress.c
  LIBS flowie_cluster_publish_stream TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_publish_ingress
  PROPERTIES LABELS "flowie;cluster;turboraft;flowmq;data-stream;publish;backpressure")

cmake_add_test(
  test_flowie_cluster_publish_egress
  SOURCES test_flowie_cluster_publish_egress.c
  LIBS flowie_cluster_publish_stream TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_publish_egress
  PROPERTIES LABELS "flowie;cluster;turboraft;flowmq;data-stream;publish;quorum")

cmake_add_test(
  test_flowie_cluster_publish_router
  SOURCES test_flowie_cluster_publish_router.c
  LIBS flowie_cluster_publish_stream TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_publish_router
  PROPERTIES LABELS "flowie;cluster;turboraft;flowmq;data-stream;publish;router")

cmake_add_test(
  test_flowie_cluster_raft_store
  SOURCES test_flowie_cluster_raft_store.c
  LIBS flowie_cluster_runtime TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_raft_store
  PROPERTIES LABELS "flowie;flowie-release;cluster;turboraft;storage;recovery")

cmake_add_test(
  test_flowie_cluster_raft_runtime
  SOURCES test_flowie_cluster_raft_runtime.c
  LIBS flowie_cluster_runtime TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_raft_runtime
  PROPERTIES LABELS "flowie;cluster;turboraft;flowmq;runtime")

cmake_add_test(
  test_flowie_cluster_raft_generation
  SOURCES test_flowie_cluster_raft_generation.c
  LIBS flowie_cluster_raft_generation TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_raft_generation
  PROPERTIES LABELS "flowie;cluster;turboraft;flowmq;composition;runtime")

if(WIN32)
  get_filename_component(_flowie_compiler_runtime_dir "${CMAKE_C_COMPILER}" DIRECTORY)
  set_tests_properties(
    test_flowie_cluster_publish_stream
    test_flowie_cluster_publish_ingress
    test_flowie_cluster_publish_egress
    test_flowie_cluster_publish_router
    test_flowie_cluster_raft_runtime
    test_flowie_cluster_raft_generation
    PROPERTIES ENVIRONMENT_MODIFICATION
               "PATH=path_list_prepend:$<TARGET_FILE_DIR:FlowMQ::FlowMQ>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboParser::DataBind>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:${_flowie_compiler_runtime_dir}")
  unset(_flowie_compiler_runtime_dir)
endif()

cmake_add_test(
  test_flowie_cluster_peer_authority
  SOURCES test_flowie_cluster_peer_authority.c
  LIBS flowie_cluster_runtime TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
cmake_target_enable_boringssl_linkage(test_flowie_cluster_peer_authority)
if(WIN32)
  target_link_libraries(test_flowie_cluster_peer_authority PRIVATE ws2_32)
endif()
set_tests_properties(
  test_flowie_cluster_peer_authority
  PROPERTIES LABELS "flowie;flowie-release;cluster;membership;security-contract")

cmake_add_test(
  test_flowie_cluster_owner_directory
  SOURCES test_flowie_cluster_owner_directory.c
  LIBS flowie_cluster_runtime TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
cmake_target_enable_boringssl_linkage(test_flowie_cluster_owner_directory)
if(WIN32)
  target_link_libraries(test_flowie_cluster_owner_directory PRIVATE ws2_32)
endif()
set_tests_properties(
  test_flowie_cluster_owner_directory
  PROPERTIES LABELS "flowie;flowie-release;cluster;ownership;security-contract")

cmake_add_test(
  test_flowie_cluster_owner_projection
  SOURCES test_flowie_cluster_owner_projection.c
  LIBS flowie_cluster_runtime TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_owner_projection
  PROPERTIES LABELS "flowie;cluster;turboraft;ownership;recovery")

cmake_add_test(
  test_flowie_cluster_state_machine
  SOURCES test_flowie_cluster_state_machine.c
  LIBS flowie_cluster_runtime TurboUtils::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/cluster ${PROJECT_SOURCE_DIR}/flowie/src
  FOLDER "cluster/tests")
set_tests_properties(
  test_flowie_cluster_state_machine
  PROPERTIES LABELS
             "flowie;flowie-release;cluster;turboraft;ownership;publish;recovery")
