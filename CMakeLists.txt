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

# Flowie is an independent MQTT product. Its broker depends only on CoroNet and
# foundational libraries; product orchestration belongs to downstream adapters.
foreach(_flowie_dependency_root IN ITEMS
        TURBOUTILS_ROOT TURBOPARSER_ROOT TURBONET_ROOT TURBODB_ROOT
        TURBOHTTP_ROOT TURBORAFT_ROOT FLOWMQ_ROOT)
  if(NOT DEFINED ENV{${_flowie_dependency_root}} OR
     "$ENV{${_flowie_dependency_root}}" STREQUAL "")
    message(FATAL_ERROR
            "Required dependency root is not defined: ${_flowie_dependency_root}")
  endif()
  if(NOT IS_DIRECTORY "$ENV{${_flowie_dependency_root}}")
    message(FATAL_ERROR
            "${_flowie_dependency_root} is not a directory: $ENV{${_flowie_dependency_root}}")
  endif()
endforeach()

find_package(TurboUtils CONFIG REQUIRED
             PATHS "$ENV{TURBOUTILS_ROOT}" NO_DEFAULT_PATH)
find_package(TurboParser CONFIG REQUIRED
             PATHS "$ENV{TURBOPARSER_ROOT}" NO_DEFAULT_PATH)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(TurboNet CONFIG REQUIRED
             PATHS "$ENV{TURBONET_ROOT}" NO_DEFAULT_PATH)
find_package(xxHash CONFIG REQUIRED)
find_package(TurboDB CONFIG REQUIRED COMPONENTS ORM
             PATHS "$ENV{TURBODB_ROOT}" NO_DEFAULT_PATH)
if(TARGET TurboParser::DataBind AND NOT TARGET TurboUtils::DataBind)
  add_library(TurboUtils::DataBind ALIAS TurboParser::DataBind)
endif()
find_package(FlowMQ CONFIG REQUIRED
             PATHS "$ENV{FLOWMQ_ROOT}" NO_DEFAULT_PATH)
find_package(TurboRaft CONFIG REQUIRED COMPONENTS Service WalStorage ServiceOwner DataStream FlowMQ
             PATHS "$ENV{TURBORAFT_ROOT}" NO_DEFAULT_PATH)
find_package(TurboHttp CONFIG REQUIRED
             PATHS "$ENV{TURBOHTTP_ROOT}" NO_DEFAULT_PATH)
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)
add_subdirectory(flowie/runtime)
add_subdirectory(flowie)
add_subdirectory(cluster)
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
