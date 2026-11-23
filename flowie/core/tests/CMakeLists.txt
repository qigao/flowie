cmake_add_test(
  test_flowie_core
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_core.c
  LIBS
  Flowie::App
  Flowie::Transport
  Flowie::Connection
  Salts::TinyTest
  INCLUDES
  ${PROJECT_SOURCE_DIR}/flowie/core/src
  FOLDER
  "flowie/core/tests")

set_tests_properties(
  test_flowie_core
  PROPERTIES LABELS "flowie;flowie-release;mqtt-fast")

cmake_add_test(
  test_flowie_security
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_security.c
  ${PROJECT_SOURCE_DIR}/flowie/core/src/flowie_security.c
  LIBS
  Salts::Core
  Salts::TinyTest
  INCLUDES
  ${PROJECT_SOURCE_DIR}/flowie/core/include
  ${PROJECT_SOURCE_DIR}/flowie/transport/include
  FOLDER
  "flowie/core/tests")
target_compile_definitions(test_flowie_security PRIVATE FLOWIE_BUILD)

set_tests_properties(
  test_flowie_security
  PROPERTIES LABELS "flowie;flowie-release;mqtt-fast;mqtt-security")

cmake_add_test(
  test_flowie_endpoint
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie.c
  LIBS
  Flowie::App
  Salts::LtvParser
  Salts::TinyTest
  INCLUDES
  ${PROJECT_SOURCE_DIR}/flowie/core/src
  FOLDER
  "flowie/core/tests")
set_tests_properties(
  test_flowie_endpoint
  PROPERTIES LABELS "flowie;flowie-release;mqtt-fast;mqtt-owner;mqtt-persistence")

cmake_add_test(
  test_flowie_ingress
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_ingress.c
  LIBS
  Flowie::App
  Salts::TinyTest
  INCLUDES
  ${PROJECT_SOURCE_DIR}/flowie/core/src
  FOLDER
  "flowie/core/tests")
set_tests_properties(
  test_flowie_ingress
  PROPERTIES LABELS "flowie;flowie-release;mqtt-fast;mqtt-ingress;backpressure")

cmake_add_test(
  test_flowie_session_delivery_index
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_session_delivery_index.c
  LIBS
  Flowie::App
  Salts::TinyTest
  FOLDER
  "flowie/core/tests")
target_include_directories(
  test_flowie_session_delivery_index
  PRIVATE ${PROJECT_SOURCE_DIR}/flowie/core/src)
set_tests_properties(
  test_flowie_session_delivery_index
  PROPERTIES LABELS "flowie;flowie-release;mqtt-fast;session")

cmake_add_test(
  test_flowie_transport_baseline
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_transport_baseline.c
  LIBS
  Flowie::App
  Flowie::Client
  GmSSL::GmSSL
  Salts::CNet
  Salts::TinyTest
  FOLDER
  "flowie/core/tests")
if(WIN32)
  target_link_libraries(test_flowie_transport_baseline PRIVATE ws2_32)
endif()

set_tests_properties(
  test_flowie_transport_baseline
  PROPERTIES
    LABELS "flowie;flowie-release;mqtt-flowie/transport;mqtt-security" TIMEOUT
    120)

cmake_add_test(
  test_flowie_mqtt_endurance
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_mqtt_endurance.c
  LIBS
  Flowie::App
  Salts::CNet
  Salts::TinyTest
  INCLUDES
  ${PROJECT_SOURCE_DIR}/flowie/core/src
  FOLDER
  "flowie/core/tests")
if(WIN32)
  target_link_libraries(test_flowie_mqtt_endurance PRIVATE ws2_32)
endif()
set_tests_properties(
  test_flowie_mqtt_endurance
  PROPERTIES
    LABELS "flowie;flowie-release;mqtt-endurance;mqtt-persistence" TIMEOUT
    300)

cmake_add_test(
  test_flowie_protocol_repository
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_protocol_repository.c
  LIBS
  Flowie::App
  flowie_orm_flow
  Salts::CNet
  Salts::TinyTest
  FOLDER
  "flowie/core/tests")

set_tests_properties(
  test_flowie_protocol_repository
  PROPERTIES LABELS "flowie;flowie-release;storage")

cmake_add_test(
  test_flowie_orm_runtime
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_orm_runtime.c
  LIBS
  flowie_orm_flow
  Salts::TinyTest
  DEFS
  FLOWIE_TEST_SQLITE_MODULE="${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}"
  FOLDER
  "flowie/core/tests")
set_tests_properties(
  test_flowie_orm_runtime
  PROPERTIES LABELS "flowie;flowie-release;storage")

if(FLOWIE_TURBODB_LIVE_TESTS)
  cmake_add_test(
    test_flowie_protocol_repository_turbodb_live
    SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_protocol_repository_turbodb_live.c
    LIBS
    Flowie::App
    flowie_orm_flow
    Salts::TinyTest
    FOLDER
    "flowie/core/tests")
  set_tests_properties(
    test_flowie_protocol_repository_turbodb_live
    PROPERTIES LABELS "flowie;live;turbodb;postgresql;storage" TIMEOUT 120)
endif()

cmake_add_test(
  test_flowie_cluster
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_cluster.c
  LIBS
  Flowie::App
  Salts::TinyTest
  FOLDER
  "flowie/core/tests")
target_include_directories(
  test_flowie_cluster
  PRIVATE ${PROJECT_SOURCE_DIR}/flowie/core/src/cluster)
set_tests_properties(
  test_flowie_cluster
  PROPERTIES LABELS "flowie;server/cluster;flowie-release")

cmake_add_test(
  test_flowie_topic_index
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_topic_index.c
  LIBS
  Flowie::App
  CMeta::CSTL
  Salts::TinyTest
  FOLDER
  "flowie/core/tests")
target_include_directories(
  test_flowie_topic_index
  PRIVATE
    ${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
    ${PROJECT_SOURCE_DIR}/flowie/core/src)
set_tests_properties(
  test_flowie_topic_index
  PROPERTIES LABELS "flowie;mqtt;topic-index")

cmake_add_test(
  test_flowie_mqtt_soak
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_mqtt_soak.c
  LIBS
  Flowie::App
  CMeta::CSTL
  Salts::TinyTest
  INCLUDES
  ${PROJECT_SOURCE_DIR}/flowie/core/src
  DEFS
  FLOWIE_SOAK_ENDURANCE_TEST="$<TARGET_FILE:test_flowie_mqtt_endurance>"
  FLOWIE_SOAK_CLIENT_CLOSE_TEST="$<TARGET_FILE:test_flowie_mqtt_client_close_full>"
  FLOWIE_SOAK_TRANSPORT_BASELINE_TEST="$<TARGET_FILE:test_flowie_transport_baseline>"
  FLOWIE_SOAK_INGRESS_TEST="$<TARGET_FILE:test_flowie_ingress>"
  FLOWIE_SOAK_RAFT_STORE_TEST="$<TARGET_FILE:test_flowie_cluster_raft_store>"
  FLOWIE_SOAK_RAFT_RUNTIME_TEST="$<TARGET_FILE:test_flowie_cluster_raft_runtime>"
  FLOWIE_SOAK_RAFT_GENERATION_TEST="$<TARGET_FILE:test_flowie_cluster_raft_generation>"
  FOLDER
  "flowie/core/tests")
if(WIN32)
  target_link_libraries(test_flowie_mqtt_soak PRIVATE psapi)
endif()
set_tests_properties(
  test_flowie_mqtt_soak
  PROPERTIES LABELS "flowie;mqtt-soak;nightly" TIMEOUT 1800)
