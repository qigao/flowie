add_executable(flowie_server flowie_server.c)
cmake_config_target(flowie_server FOLDER "flowie/server")
target_link_libraries(flowie_server PRIVATE Flowie::App TurboUtils::Core)
cmake_target_enable_boringssl_linkage(flowie_server)

install(TARGETS flowie_server RUNTIME DESTINATION bin)
