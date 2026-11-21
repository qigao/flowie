cmake_minimum_required(VERSION 3.27)

if(POLICY CMP0144)
	cmake_policy(SET CMP0144 OLD)
endif()
if(POLICY CMP0167)
	cmake_policy(SET CMP0167 OLD)
endif()

project(Flowie VERSION 1.0.5 LANGUAGES C CXX)

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
	# MSVC accepts this spelling; it is preserved by the ccache launcher.
	add_compile_options(-experimental:c11atomics)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMakeOptions.cmake")
include(CTest)
if(FLOWIE_BUILD_BENCHMARKS)
	enable_testing()
endif()
include(Sanitizers)
include(CmakeUtils)
include(FlowieDependencies)

if(FLOWIE_BUILD_TESTS)
	if(UNIX)
		add_test(
			NAME flowie_server_docker_entrypoint_contract
			COMMAND sh ${CMAKE_SOURCE_DIR}/deploy/server/tests/test-docker-entrypoint.sh
		)
		set_tests_properties(flowie_server_docker_entrypoint_contract
			PROPERTIES
				LABELS "flowie;build-contract;docker;security-contract"
				TIMEOUT 30
		)
	endif()

endif()

include(FindTools)
add_subdirectory(vendor)

add_subdirectory(flowie)
include(FlowieRuntime)
include(FlowieInstall)
