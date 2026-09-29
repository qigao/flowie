cmake_minimum_required(VERSION 3.25)

project(FlowieConnectionCI LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)

if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must name Salts 1.8.3")
endif()
if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR "$ENV{SALTS_UTILS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_UTILS_ROOT must name SaltsUtils 4.1.3")
endif()
if(NOT DEFINED ENV{HTTP_SERVICES_ROOT} OR "$ENV{HTTP_SERVICES_ROOT}" STREQUAL "")
  message(FATAL_ERROR "HTTP_SERVICES_ROOT must name CHttp 1.1.5")
endif()

find_package(Salts 1.8.3 EXACT CONFIG REQUIRED
             PATHS "$ENV{SALTS_ROOT}" NO_DEFAULT_PATH)
find_package(SaltsUtils 4.1.3 EXACT CONFIG REQUIRED
             PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH)
find_package(Chttp 1.1.5 EXACT CONFIG REQUIRED
             PATHS "$ENV{HTTP_SERVICES_ROOT}" NO_DEFAULT_PATH)

get_filename_component(FLOWIE_SOURCE_DIR
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_library(flowie_connection_ci STATIC
  "${FLOWIE_SOURCE_DIR}/connection/src/flowie_connection.c"
  "${FLOWIE_SOURCE_DIR}/connection/include/flowie_connection.h")

target_include_directories(flowie_connection_ci
  PUBLIC "${FLOWIE_SOURCE_DIR}/connection/include")
target_link_libraries(flowie_connection_ci
  PUBLIC Salts::Core Salts::CNet CHttp::Server)

add_executable(test_flowie_connection_ci
  "${FLOWIE_SOURCE_DIR}/connection/tests/test_flowie_connection.c")
target_link_libraries(test_flowie_connection_ci
  PRIVATE flowie_connection_ci CHttp::Client CHttp::Server
          Salts::CNet Salts::TinyTest)

add_test(NAME flowie_connection_ci COMMAND test_flowie_connection_ci)
set_tests_properties(flowie_connection_ci PROPERTIES
  LABELS "flowie;connection;cnet;chttp;sg")
