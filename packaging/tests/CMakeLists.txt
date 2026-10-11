cmake_minimum_required(VERSION 3.25)
project(FlowieInstalledConsumer LANGUAGES C CXX)

find_package(Flowie 1.1 CONFIG REQUIRED
             PATHS "$ENV{FLOWIE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
add_executable(flowie_release_consumer main.c)
target_compile_features(flowie_release_consumer PRIVATE c_std_11)
target_link_libraries(flowie_release_consumer PRIVATE
                      Flowie::Flowie Flowie::Client Flowie::Protocol)

enable_testing()
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME flowie_installed_consumer COMMAND flowie_release_consumer)
  set_tests_properties(flowie_installed_consumer PROPERTIES TIMEOUT 30)
endif()
