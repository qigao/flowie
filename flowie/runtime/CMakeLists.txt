add_library(
  flowie_transport STATIC
  src/flow_connection.c
  src/flow_execution.c
  src/flow_io_policy.c
  include/flow_connection.h
  include/flow_execution.h
  include/flow_io_policy.h
  ../include/flowie_execution.h)

cmake_config_target(
  flowie_transport
  ALIAS Flowie::Transport
  NO_INSTALL
  FOLDER "flowie/runtime")

set_target_properties(flowie_transport PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  flowie_transport
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>")
target_link_libraries(
  flowie_transport
  PUBLIC Salts::Core Salts::Coroutine)

add_library(
  flowie_net_runtime STATIC
  src/flow_net_runtime.c
  include/flow_net_runtime.h)

cmake_config_target(
  flowie_net_runtime
  ALIAS Flowie::NetRuntime
  NO_INSTALL
  FOLDER "flowie/runtime")

set_target_properties(flowie_net_runtime PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  flowie_net_runtime
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(
  flowie_net_runtime
  PUBLIC Salts::Core Salts::CNet Salts::CHTTP)

if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
