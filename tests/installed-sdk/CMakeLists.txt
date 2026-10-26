cmake_minimum_required(VERSION 3.25)
project(FlowieInstalledConsumer LANGUAGES C CXX)

# All imports must resolve from the installed SDKs only. No source-tree
# targets or fallback to an older Salts/CHttp/TurboDB ABI is allowed.
find_package(Flowie CONFIG REQUIRED PATHS "$ENV{FLOWIE_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Flowie::Flowie OR NOT TARGET Flowie::Client OR
   NOT TARGET Flowie::Protocol OR NOT TARGET Salts::CNet)
  message(FATAL_ERROR "Flowie installed C11/C++17 SDK is missing canonical exports")
endif()

add_executable(flowie_consumer_c11 consumer.c)
set_target_properties(flowie_consumer_c11 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(flowie_consumer_c11 PRIVATE
                      Flowie::Flowie Flowie::Client Flowie::Protocol Salts::CNet)

add_executable(flowie_consumer_cpp17 consumer.cpp)
set_target_properties(flowie_consumer_cpp17 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(flowie_consumer_cpp17 PRIVATE
                      Flowie::Flowie Flowie::Client Flowie::Protocol Salts::CNet)

# The installed Windows runtime admission is a distinct consumer contract:
# it loads the exact shipped PostgreSQL DSO through the PUBLIC TurboDB C ABI,
# not private Flowie source helpers or a relaxed PATH-based loader.
if(WIN32)
  if(NOT DEFINED ENV{TURBODB_ROOT} OR NOT DEFINED ENV{SALTS_ROOT})
    message(FATAL_ERROR "Exact installed TurboDB and Salts SDK roots are required")
  endif()
  find_package(TurboDB CONFIG REQUIRED PATHS "$ENV{TURBODB_ROOT}"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(NOT TARGET Orm::C)
    message(FATAL_ERROR "Installed TurboDB Orm::C target is missing")
  endif()
  foreach(_file IN ITEMS
      "$ENV{TURBODB_ROOT}/lib/turbodb/drivers/turbodb_driver_postgresql.dll"
      "$ENV{TURBODB_ROOT}/bin/libpq.dll"
      "$ENV{SALTS_ROOT}/bin/cnet.dll"
      "$ENV{SALTS_ROOT}/bin/salts.dll")
    if(NOT EXISTS "${_file}")
      message(FATAL_ERROR "Installed PostgreSQL Plugin dependency is missing: ${_file}")
    endif()
  endforeach()
  add_executable(flowie_consumer_pg_installed consumer_postgresql_plugin.c)
  set_target_properties(flowie_consumer_pg_installed PROPERTIES
                        C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_link_libraries(flowie_consumer_pg_installed PRIVATE Orm::C)
  target_compile_definitions(flowie_consumer_pg_installed PRIVATE
    FLOWIE_TEST_INSTALLED_PG_PLUGIN="$ENV{TURBODB_ROOT}/lib/turbodb/drivers/turbodb_driver_postgresql.dll"
    FLOWIE_TEST_INSTALLED_TURBODB_BIN="$ENV{TURBODB_ROOT}/bin"
    FLOWIE_TEST_INSTALLED_SALTS_BIN="$ENV{SALTS_ROOT}/bin")
  target_compile_options(flowie_consumer_pg_installed PRIVATE /W4 /WX)
endif()

enable_testing()
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME flowie_installed_c11 COMMAND flowie_consumer_c11)
  add_test(NAME flowie_installed_cpp17 COMMAND flowie_consumer_cpp17)
  if(WIN32)
    add_test(NAME flowie_installed_postgresql_plugin
             COMMAND flowie_consumer_pg_installed)
  endif()
endif()
