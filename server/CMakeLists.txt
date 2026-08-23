add_executable(flowie_server flowie_server.c)
target_link_libraries(flowie_server PRIVATE Flowie::App)
