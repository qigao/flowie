if(WIN32)
  if(MINGW)
    set(_flowie_sqlite_runtime_name libsqlite3.dll)
  else()
    set(_flowie_sqlite_runtime_name sqlite3.dll)
  endif()
  find_file(
    FLOWIE_SQLITE_RUNTIME "${_flowie_sqlite_runtime_name}"
    REQUIRED NO_DEFAULT_PATH
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  find_file(
    FLOWIE_POSTGRESQL_RUNTIME libpq.dll
    REQUIRED NO_DEFAULT_PATH
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  set(_flowie_driver_dlls
      "${FLOWIE_SQLITE_RUNTIME}" "${FLOWIE_POSTGRESQL_RUNTIME}"
      "$<TARGET_FILE:Salts::CNet>" "$<TARGET_FILE:Salts::Crypto>")
  set(_flowie_runtime_directories "$<TARGET_FILE_DIR:flowie>")
  if(FLOWIE_BUILD_CONTROL AND BUILD_TESTS)
    list(
      APPEND _flowie_runtime_directories
      "${CMAKE_BINARY_DIR}/flowie/control/test-bin/$<$<BOOL:${CMAKE_CONFIGURATION_TYPES}>:$<CONFIG>>"
    )
  endif()
  add_custom_target(flowie_driver_runtime ALL)
  foreach(_flowie_runtime_directory IN LISTS _flowie_runtime_directories)
    add_custom_command(
      TARGET flowie_driver_runtime
      POST_BUILD
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory "${_flowie_runtime_directory}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different ${_flowie_driver_dlls}
        "${_flowie_runtime_directory}"
      COMMAND_EXPAND_LISTS
      VERBATIM)
  endforeach()
  add_dependencies(flowie_orm_flow flowie_driver_runtime)
  install(FILES ${_flowie_driver_dlls} DESTINATION bin)
endif()
