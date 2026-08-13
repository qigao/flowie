cmake_minimum_required(VERSION 3.20)
project(FlowiePackageConsumer LANGUAGES C)

find_package(Flowie CONFIG REQUIRED)

add_executable(flowie_package_consumer main.c)
target_link_libraries(flowie_package_consumer PRIVATE Flowie::Flowie Flowie::Client)
