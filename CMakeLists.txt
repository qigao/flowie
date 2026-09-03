cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 OLD)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

project(Flowie VERSION 1.0.0 LANGUAGES C CXX)

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
include(Sanitizers)
include(CmakeUtils)

if(FLOWIE_BUILD_TESTS)
  add_test(
    NAME flowie_standalone_dockerfile_contract
    COMMAND
      ${CMAKE_COMMAND}
      -DFLOWIE_DOCKERFILE=${CMAKE_SOURCE_DIR}/deploy/server/Dockerfile
      -P ${CMAKE_SOURCE_DIR}/cmake/tests/VerifyStandaloneDockerfile.cmake)
  set_tests_properties(
    flowie_standalone_dockerfile_contract
    PROPERTIES LABELS "flowie;build-contract;cluster;docker" TIMEOUT 30)

  if(UNIX)
    add_test(NAME flowie_server_docker_entrypoint_contract
             COMMAND sh ${CMAKE_SOURCE_DIR}/deploy/server/tests/test-docker-entrypoint.sh)
    set_tests_properties(
      flowie_server_docker_entrypoint_contract
      PROPERTIES LABELS "flowie;build-contract;docker;security-contract" TIMEOUT 30)
  endif()

  add_test(
    NAME flowie_default_no_cluster_configure
    COMMAND
      ${CMAKE_COMMAND}
      -DFLOWIE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DFLOWIE_BINARY_DIR=${CMAKE_BINARY_DIR}/default-no-cluster-configure
      -DFLOWIE_GENERATOR=${CMAKE_GENERATOR}
      -DFLOWIE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DFLOWIE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
      -DFLOWIE_VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}
      -DFLOWIE_VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}
      -DFLOWIE_SALTS_ROOT=$ENV{SALTS_ROOT}
      -DFLOWIE_SALTS_UTILS_ROOT=$ENV{SALTS_UTILS_ROOT}
      -DFLOWIE_TURBONET_ROOT=$ENV{TURBONET_ROOT}
      -DFLOWIE_TURBODB_ROOT=$ENV{TURBODB_ROOT}
      -P ${CMAKE_SOURCE_DIR}/cmake/tests/VerifyDefaultNoCluster.cmake)
  set_tests_properties(
    flowie_default_no_cluster_configure
    PROPERTIES LABELS "flowie;build-contract;cluster" TIMEOUT 300)
endif()

 
if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must name the installed Salts profile")
endif()
if(NOT IS_DIRECTORY "$ENV{SALTS_ROOT}")
  message(FATAL_ERROR "SALTS_ROOT is not a directory: $ENV{SALTS_ROOT}")
endif()
find_package(Salts CONFIG REQUIRED
             PATHS "$ENV{SALTS_ROOT}" NO_DEFAULT_PATH)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
if(FLOWIE_BUILD_CONTROL)
  if(NOT DEFINED ENV{TURBONET_ROOT} OR "$ENV{TURBONET_ROOT}" STREQUAL "")
    message(FATAL_ERROR "TURBONET_ROOT is required for the legacy Ed448 crypto archive")
  endif()
  find_library(FLOWIE_ED448_CRYPTO_LIBRARY NAMES turbo_crypto
               PATHS "$ENV{TURBONET_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  add_library(flowie_ed448_crypto STATIC IMPORTED GLOBAL)
  add_library(Flowie::Ed448Crypto ALIAS flowie_ed448_crypto)
  set_target_properties(flowie_ed448_crypto PROPERTIES
                        IMPORTED_LOCATION "${FLOWIE_ED448_CRYPTO_LIBRARY}")
endif()
find_package(xxHash CONFIG REQUIRED)
find_package(Orm CONFIG REQUIRED
             PATHS "$ENV{TURBODB_ROOT}" NO_DEFAULT_PATH)
if(FLOWIE_BUILD_CLUSTER)
  find_package(FlowMQ CONFIG REQUIRED
               PATHS "$ENV{FLOWMQ_ROOT}" NO_DEFAULT_PATH)
  find_package(
    TurboRaft CONFIG REQUIRED COMPONENTS Service WalStorage DataStream FlowMQ
    PATHS "$ENV{TURBORAFT_ROOT}" NO_DEFAULT_PATH)
endif()
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)
add_subdirectory(flowie/runtime)
add_subdirectory(flowie)
if(FLOWIE_BUILD_CLUSTER)
  add_subdirectory(cluster)
endif()
add_subdirectory(server)
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
