add_library(
  flowie_connection
  STATIC src/flowie_connection.c src/flowie_send_batch.c src/flowie_affinity.c)
cmake_config_target(flowie_connection ALIAS Flowie::Connection NO_INSTALL FOLDER
                    "flowie/transport/internal")
set_target_properties(flowie_connection PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  flowie_connection
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_link_libraries(
  flowie_connection
  PUBLIC Salts::Core Salts::CNet CHttp::App)

add_library(
  flowie_transport
  STATIC
    src/flow_connection.c
    src/flow_execution.c
    src/flow_io_policy.c
    include/flow_connection.h
    include/flow_execution.h
    include/flow_io_policy.h
    include/flowie_execution.h)

cmake_config_target(flowie_transport ALIAS Flowie::Transport NO_INSTALL FOLDER
                    "transport")

set_target_properties(flowie_transport PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  flowie_transport
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(flowie_transport PUBLIC Salts::Core Salts::Coroutine)

if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
