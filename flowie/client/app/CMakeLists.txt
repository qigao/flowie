enable_language(RC)
include(FetchContent)
set(_openwtl_version "$ENV{OPENWTL_VERSION}")
set(_openwtl_sha256 "$ENV{OPENWTL_SHA256}")
if(NOT
	_openwtl_version
	MATCHES
	"^[0-9]+\\.[0-9]+\\.[0-9]+$"
	OR
	NOT
	_openwtl_sha256
	MATCHES
	"^[0-9a-fA-F]+$"
)
	message(FATAL_ERROR
		"Load OPENWTL_VERSION and OPENWTL_SHA256 from .env before configuring Windows"
	)
endif()
fetchcontent_declare(
	OpenWTL
	URL
	"https://github.com/qigao/OpenWTL/archive/refs/tags/v${_openwtl_version}.zip"
	URL_HASH
	"SHA256=${_openwtl_sha256}"
	DOWNLOAD_EXTRACT_TIMESTAMP
	TRUE
	SYSTEM
)
fetchcontent_makeavailable(OpenWTL)

file(GLOB _workbench_model
	CONFIGURE_DEPENDS
	src/workbench*.cpp
	src/workbench*.h
)
add_library(flowie_workbench_model STATIC ${_workbench_model})
cmake_config_target(
	flowie_workbench_model
	NO_INSTALL
	FOLDER
	"flowie/client/app"
)
target_include_directories(flowie_workbench_model
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(flowie_workbench_model
	PUBLIC
		Flowie::Client
		Salts::Core
		Salts::JsonParser
	PRIVATE
		CHttp::Client
		Salts::CNet
		Salts::DateTimeParser
		Salts::Serial
		Salts::URI
)
file(GLOB _workbench_ui
	CONFIGURE_DEPENDS
	src/main_*.cpp
	src/main_*.h
	src/ui/*.cpp
	src/ui/*.h
)
source_group(TREE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	FILES
	${_workbench_model}
	${_workbench_ui}
)
add_library(flowie_workbench_ui STATIC ${_workbench_ui})
cmake_config_target(flowie_workbench_ui NO_INSTALL FOLDER "flowie/client/app")
target_link_libraries(flowie_workbench_ui
	PUBLIC
		flowie_workbench_model
		OpenWTL::WTL
	PRIVATE
		Salts::IniParser
)
target_compile_definitions(flowie_workbench_ui PUBLIC NOMINMAX)
# workbench.rc embeds the manifest for the application and the window tests.
cmake_add_executable(
	flowie_mqtt_workbench
	SOURCES
	src/main.cpp
	src/workbench.rc
	LIBS
	flowie_workbench_ui
	FOLDER
	"flowie/client/app"
)
set_property(
	SOURCE src/workbench.rc
	PROPERTY
		OBJECT_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/src/workbench.manifest"
)
set_target_properties(flowie_mqtt_workbench PROPERTIES WIN32_EXECUTABLE TRUE)
install(TARGETS flowie_mqtt_workbench RUNTIME DESTINATION bin)
target_compile_definitions(flowie_mqtt_workbench PRIVATE NOMINMAX)
target_include_directories(flowie_mqtt_workbench
	PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
set(_workbench_crt)
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	foreach(_dll IN ITEMS libc++.dll libunwind.dll libwinpthread-1.dll)
		list(APPEND _workbench_crt
			"$ENV{LLVM_MINGW_ROOT}/$ENV{LLVM_MINGW_TARGET}/bin/${_dll}"
		)
	endforeach()
endif()
add_custom_command(
	TARGET flowie_mqtt_workbench
	POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		$<TARGET_RUNTIME_DLLS:flowie_mqtt_workbench> ${_workbench_crt}
		"$<TARGET_FILE_DIR:flowie_mqtt_workbench>"
	COMMAND_EXPAND_LISTS VERBATIM
)
install(
	FILES
		$<TARGET_RUNTIME_DLLS:flowie_mqtt_workbench>
		${_workbench_crt}
	DESTINATION bin
)
if(MSVC)
	target_link_options(flowie_mqtt_workbench PRIVATE /MANIFEST:NO)
endif()
install(
	FILES "${openwtl_SOURCE_DIR}/LICENSE"
	DESTINATION share/licenses/Flowie/OpenWTL
)
install(
	FILES "${openwtl_SOURCE_DIR}/wtl/MS-PL.txt"
	DESTINATION share/licenses/Flowie/OpenWTL
)
if(BUILD_TESTS)
	add_subdirectory(tests)
	set_property(
		SOURCE
			"${CMAKE_CURRENT_SOURCE_DIR}/src/workbench.rc"
			TARGET_DIRECTORY
			test_flowie_workbench
		PROPERTY
			OBJECT_DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/src/workbench.manifest"
	)
endif()
