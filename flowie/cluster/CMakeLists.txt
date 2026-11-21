if(NOT FLOWIE_BUILD_CLUSTER)
  return()
endif()

add_library(
  flowie_cluster_peer
  STATIC
    src/flowie_cluster_peer_connect_bind.c
    src/flowie_cluster_peer_connection_lost.c
    src/flowie_cluster_peer_edge_action.c
    src/flowie_cluster_peer_mqtt_command.c
    src/flowie_cluster_peer_publish_settle.c
    src/flowie_cluster_peer_mqtt_reply.c
    src/flowie_cluster_peer_protocol.c
    src/flowie_cluster_peer_registry.c
    src/flowie_cluster_peer_takeover_close.c
    src/flowie_cluster_peer_transport.c
    src/flowie_cluster_peer_internal.h
    src/flowie_cluster_peer_wire_internal.h)
cmake_config_target(flowie_cluster_peer NO_INSTALL FOLDER "flowie/cluster")
target_include_directories(
  flowie_cluster_peer
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/flowie/core/src
    ${PROJECT_SOURCE_DIR}/flowie/core/src/cluster ${FLOWIE_PRIVATE_INCLUDE_DIR})
target_link_libraries(
  flowie_cluster_peer
  PUBLIC Flowie::App Salts::CNet
  PRIVATE Salts::Core CMeta::CSTL)

add_library(
  flowie_cluster_topology
  STATIC src/flowie_cluster_topology.c src/flowie_cluster_topology_internal.h)
cmake_config_target(flowie_cluster_topology NO_INSTALL FOLDER "flowie/cluster")
target_include_directories(
  flowie_cluster_topology
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/flowie/core/src
    ${PROJECT_SOURCE_DIR}/flowie/core/src/cluster ${FLOWIE_PRIVATE_INCLUDE_DIR})
target_link_libraries(
  flowie_cluster_topology
  PUBLIC Flowie::App
  PRIVATE Salts::Core CMeta::CSTL)

add_library(
  flowie_cluster_publish_event
  STATIC
    src/flowie_cluster_publish_event.c
    src/flowie_cluster_publish_event_internal.h)
cmake_config_target(flowie_cluster_publish_event NO_INSTALL FOLDER
                    "flowie/cluster")
target_include_directories(
  flowie_cluster_publish_event
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/flowie/core/src
    ${PROJECT_SOURCE_DIR}/flowie/core/src/cluster ${FLOWIE_PRIVATE_INCLUDE_DIR})
target_link_libraries(
  flowie_cluster_publish_event
  PUBLIC flowie_cluster_peer Flowie::App
  PRIVATE Salts::Core)

add_library(
  flowie_cluster_publish_stream
  STATIC
    src/flowie_cluster_publish_stream.c
    src/flowie_cluster_publish_stream_internal.h
    src/flowie_cluster_publish_ingress.c
    src/flowie_cluster_publish_ingress_internal.h
    src/flowie_cluster_publish_egress.c
    src/flowie_cluster_publish_egress_internal.h
    src/flowie_cluster_publish_router.c
    src/flowie_cluster_publish_router_internal.h)
cmake_config_target(flowie_cluster_publish_stream NO_INSTALL FOLDER
                    "flowie/cluster")
target_include_directories(
  flowie_cluster_publish_stream
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/flowie/core/src
    ${PROJECT_SOURCE_DIR}/flowie/core/src/cluster ${FLOWIE_PRIVATE_INCLUDE_DIR})
target_link_libraries(
  flowie_cluster_publish_stream
  PUBLIC flowie_cluster_publish_event TurboRaft::DataStream
  PRIVATE Salts::Core CMeta::CSTL)

add_library(
  flowie_cluster_runtime
  STATIC
    src/flowie_cluster_raft_store.c
    src/flowie_cluster_raft_store_internal.h
    src/flowie_cluster_raft_runtime.c
    src/flowie_cluster_raft_runtime_internal.h
    src/flowie_cluster_owner_directory.c
    src/flowie_cluster_owner_directory_internal.h
    src/flowie_cluster_owner_projection.c
    src/flowie_cluster_owner_projection_internal.h
    src/flowie_cluster_state_machine.c
    src/flowie_cluster_state_machine_internal.h
    src/flowie_cluster_peer_authority.c
    src/flowie_cluster_peer_authority_internal.h)
cmake_config_target(flowie_cluster_runtime NO_INSTALL FOLDER "flowie/cluster")
target_include_directories(
  flowie_cluster_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/flowie/core/src
    ${PROJECT_SOURCE_DIR}/flowie/core/src/cluster ${FLOWIE_PRIVATE_INCLUDE_DIR})
target_link_libraries(
  flowie_cluster_runtime
  PUBLIC
    flowie_cluster_peer
    Flowie::App
    Flowie::Transport
    TurboRaft::Service
    TurboRaft::WalStorage
    TurboRaft::CNet
    TurboRaft::DataStream
    Salts::Core
  PRIVATE CMeta::CSTL monocypher)

target_link_libraries(
  flowie_cluster_publish_stream
  PUBLIC flowie_cluster_runtime)

add_library(
  flowie_cluster_raft_generation
  STATIC
    src/flowie_cluster_raft_generation.c
    src/flowie_cluster_raft_generation_internal.h)
cmake_config_target(flowie_cluster_raft_generation NO_INSTALL FOLDER
                    "flowie/cluster")
target_include_directories(
  flowie_cluster_raft_generation
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/flowie/core/src
    ${PROJECT_SOURCE_DIR}/flowie/core/src/cluster ${FLOWIE_PRIVATE_INCLUDE_DIR})
target_link_libraries(
  flowie_cluster_raft_generation
  PUBLIC flowie_cluster_runtime flowie_cluster_publish_stream)

if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
