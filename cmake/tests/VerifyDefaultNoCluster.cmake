foreach(_required IN ITEMS FLOWIE_SOURCE_DIR FLOWIE_BINARY_DIR FLOWIE_GENERATOR
                           FLOWIE_SALTS_ROOT FLOWIE_SALTS_UTILS_ROOT
                           FLOWIE_TURBODB_ROOT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

set(ENV{SALTS_ROOT} "${FLOWIE_SALTS_ROOT}")
set(ENV{SALTS_UTILS_ROOT} "${FLOWIE_SALTS_UTILS_ROOT}")
set(ENV{TURBODB_ROOT} "${FLOWIE_TURBODB_ROOT}")

unset(ENV{FLOWMQ_ROOT})
unset(ENV{TURBORAFT_ROOT})

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${FLOWIE_SOURCE_DIR}"
    -B "${FLOWIE_BINARY_DIR}"
    -G "${FLOWIE_GENERATOR}"
    -UFLOWIE_BUILD_CLUSTER
    -DFLOWIE_BUILD_TESTS=OFF)

if(DEFINED FLOWIE_BUILD_TYPE AND NOT "${FLOWIE_BUILD_TYPE}" STREQUAL "")
  list(APPEND _configure_command "-DCMAKE_BUILD_TYPE=${FLOWIE_BUILD_TYPE}")
endif()
if(DEFINED FLOWIE_TOOLCHAIN_FILE AND NOT "${FLOWIE_TOOLCHAIN_FILE}" STREQUAL "")
  list(APPEND _configure_command "-DCMAKE_TOOLCHAIN_FILE=${FLOWIE_TOOLCHAIN_FILE}")
endif()
if(DEFINED FLOWIE_VCPKG_INSTALLED_DIR AND
   NOT "${FLOWIE_VCPKG_INSTALLED_DIR}" STREQUAL "")
  list(APPEND _configure_command
       "-DVCPKG_INSTALLED_DIR=${FLOWIE_VCPKG_INSTALLED_DIR}")
endif()
if(DEFINED FLOWIE_VCPKG_TARGET_TRIPLET AND
   NOT "${FLOWIE_VCPKG_TARGET_TRIPLET}" STREQUAL "")
  list(APPEND _configure_command
       "-DVCPKG_TARGET_TRIPLET=${FLOWIE_VCPKG_TARGET_TRIPLET}")
endif()

execute_process(
  COMMAND ${_configure_command}
  RESULT_VARIABLE _configure_result
  OUTPUT_VARIABLE _configure_stdout
  ERROR_VARIABLE _configure_stderr)

if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR
          "Default no-Cluster configure failed (${_configure_result})\n"
          "stdout:\n${_configure_stdout}\n"
          "stderr:\n${_configure_stderr}")
endif()

file(STRINGS "${FLOWIE_BINARY_DIR}/CMakeCache.txt" _cluster_cache
     REGEX "^FLOWIE_BUILD_CLUSTER:BOOL=")
if(NOT _cluster_cache STREQUAL "FLOWIE_BUILD_CLUSTER:BOOL=OFF")
  message(FATAL_ERROR
          "Expected default FLOWIE_BUILD_CLUSTER=OFF, got: ${_cluster_cache}")
endif()
