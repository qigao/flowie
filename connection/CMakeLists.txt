add_library(
  flowie_connection STATIC
  src/flowie_connection.c
  include/flowie_connection.h)

cmake_config_target(
  flowie_connection
  ALIAS Flowie::Connection
  NO_INSTALL
  FOLDER "connection")

set_target_properties(flowie_connection PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  flowie_connection
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(
  flowie_connection
  PUBLIC Salts::Core Salts::CNet CHttp::Server)

if(FLOWIE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
