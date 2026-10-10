cmake_minimum_required(VERSION 3.25)
project(FlowieInstalledConsumer LANGUAGES C CXX)

# All imports must resolve from the installed SDKs only. No source-tree
# targets or fallback to an older Salts/CHttp/TurboDB ABI is allowed.
find_package(Flowie CONFIG REQUIRED)
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

enable_testing()
add_test(NAME flowie_installed_c11 COMMAND flowie_consumer_c11)
add_test(NAME flowie_installed_cpp17 COMMAND flowie_consumer_cpp17)
