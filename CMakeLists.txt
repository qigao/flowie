cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 OLD)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

project(Flowie VERSION 1.0.1 LANGUAGES C CXX)

set(PROJECT_DESCRIPTION "Flowie MQTT server and client library")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/flowie")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMakeOptions.cmake")
include(CTest)
if(FLOWIE_BUILD_BENCHMARKS)
  enable_testing()
endif()
include(Sanitizers)
include(CmakeUtils)

if(FLOWIE_BUILD_TESTS)
  if(UNIX)
    add_test(NAME flowie_server_docker_entrypoint_contract
             COMMAND sh ${CMAKE_SOURCE_DIR}/deploy/server/tests/test-docker-entrypoint.sh)
    set_tests_properties(
      flowie_server_docker_entrypoint_contract
      PROPERTIES LABELS "flowie;build-contract;docker;security-contract" TIMEOUT 30)
  endif()

endif()


if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must name the installed Salts profile")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" FLOWIE_SALTS_ROOT_PATH)
set(ENV{SALTS_ROOT} "${FLOWIE_SALTS_ROOT_PATH}")
if(NOT IS_DIRECTORY "${FLOWIE_SALTS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_ROOT is not a directory: $ENV{SALTS_ROOT}")
endif()
find_package(Salts 2.3.0 CONFIG REQUIRED
             PATHS "${FLOWIE_SALTS_ROOT_PATH}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR "$ENV{SALTS_UTILS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_UTILS_ROOT must name the installed SaltsUtils profile")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_UTILS_ROOT}" FLOWIE_SALTS_UTILS_ROOT_PATH)
set(ENV{SALTS_UTILS_ROOT} "${FLOWIE_SALTS_UTILS_ROOT_PATH}")
if(NOT IS_DIRECTORY "${FLOWIE_SALTS_UTILS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_UTILS_ROOT is not a directory: $ENV{SALTS_UTILS_ROOT}")
endif()
find_package(SaltsUtils 4.3.0 CONFIG REQUIRED
             PATHS "${FLOWIE_SALTS_UTILS_ROOT_PATH}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT DEFINED ENV{HTTP_SERVICES_ROOT} OR "$ENV{HTTP_SERVICES_ROOT}" STREQUAL "")
  message(FATAL_ERROR "HTTP_SERVICES_ROOT must name the installed Chttp profile")
endif()
file(TO_CMAKE_PATH "$ENV{HTTP_SERVICES_ROOT}" FLOWIE_HTTP_SERVICES_ROOT_PATH)
if(NOT IS_DIRECTORY "${FLOWIE_HTTP_SERVICES_ROOT_PATH}")
  message(FATAL_ERROR
          "HTTP_SERVICES_ROOT is not a directory: $ENV{HTTP_SERVICES_ROOT}")
endif()
find_package(Chttp 2.1.0 CONFIG REQUIRED
             PATHS "${FLOWIE_HTTP_SERVICES_ROOT_PATH}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(xxHash CONFIG REQUIRED)
if(NOT DEFINED ENV{TURBODB_ROOT} OR "$ENV{TURBODB_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TURBODB_ROOT must name the installed TurboDB profile")
endif()
file(TO_CMAKE_PATH "$ENV{TURBODB_ROOT}" FLOWIE_TURBODB_ROOT_PATH)
if(NOT IS_DIRECTORY "${FLOWIE_TURBODB_ROOT_PATH}")
  message(FATAL_ERROR "TURBODB_ROOT is not a directory: $ENV{TURBODB_ROOT}")
endif()
find_package(TurboDB 2.3.2 CONFIG REQUIRED
             PATHS "${FLOWIE_TURBODB_ROOT_PATH}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(FLOWIE_BUILD_CLUSTER)
  if(NOT DEFINED ENV{FLOWMQ_ROOT} OR "$ENV{FLOWMQ_ROOT}" STREQUAL "")
    message(FATAL_ERROR "FLOWMQ_ROOT must name the installed FlowMQ profile")
  endif()
  if(NOT DEFINED ENV{TURBORAFT_ROOT} OR "$ENV{TURBORAFT_ROOT}" STREQUAL "")
    message(FATAL_ERROR "TURBORAFT_ROOT must name the installed TurboRaft profile")
  endif()
  file(TO_CMAKE_PATH "$ENV{FLOWMQ_ROOT}" FLOWIE_FLOWMQ_ROOT_PATH)
  file(TO_CMAKE_PATH "$ENV{TURBORAFT_ROOT}" FLOWIE_TURBORAFT_ROOT_PATH)
  if(NOT IS_DIRECTORY "${FLOWIE_FLOWMQ_ROOT_PATH}")
    message(FATAL_ERROR "FLOWMQ_ROOT is not a directory: $ENV{FLOWMQ_ROOT}")
  endif()
  if(NOT IS_DIRECTORY "${FLOWIE_TURBORAFT_ROOT_PATH}")
    message(FATAL_ERROR "TURBORAFT_ROOT is not a directory: $ENV{TURBORAFT_ROOT}")
  endif()
  find_package(FlowMQ CONFIG REQUIRED
               PATHS "${FLOWIE_FLOWMQ_ROOT_PATH}" NO_DEFAULT_PATH)
  find_package(
    TurboRaft CONFIG REQUIRED COMPONENTS Service WalStorage DataStream FlowMQ
    PATHS "${FLOWIE_TURBORAFT_ROOT_PATH}" NO_DEFAULT_PATH)
endif()
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)
add_subdirectory(transport)
add_subdirectory(connection)
add_subdirectory(protocol)
add_subdirectory(client)
add_subdirectory(flowie)
if(FLOWIE_BUILD_CLUSTER)
  add_subdirectory(cluster)
endif()
if(FLOWIE_BUILD_SERVER)
  add_subdirectory(server)
endif()
if(FLOWIE_BUILD_CONTROL)
  add_subdirectory(control)
endif()
set(FLOWIE_EXPORT_TARGETS flowie_protocol flowie_client flowie)

install(
  TARGETS ${FLOWIE_EXPORT_TARGETS}
  EXPORT FlowieTargets
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  INCLUDES DESTINATION include)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FlowieConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/FlowieConfig.cmake"
  INSTALL_DESTINATION "lib/cmake/Flowie")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FlowieConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/FlowieConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/FlowieConfigVersion.cmake"
  DESTINATION "lib/cmake/Flowie")
install(
  EXPORT FlowieTargets
  FILE FlowieTargets.cmake
  NAMESPACE Flowie::
  DESTINATION "lib/cmake/Flowie")

export(
  EXPORT FlowieTargets
  FILE "${CMAKE_CURRENT_BINARY_DIR}/FlowieTargets.cmake"
  NAMESPACE Flowie::)
