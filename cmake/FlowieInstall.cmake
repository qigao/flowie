install(
    TARGETS flowie_protocol flowie_client flowie
    EXPORT FlowieTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    INCLUDES
    DESTINATION include
)

include(CMakePackageConfigHelpers)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/NOTICE"
              "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" DESTINATION share/licenses/Flowie
)
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/FlowieConfig.cmake.in" "${PROJECT_BINARY_DIR}/FlowieConfig.cmake"
    INSTALL_DESTINATION lib/cmake/Flowie
)
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/FlowieConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(FILES "${PROJECT_BINARY_DIR}/FlowieConfig.cmake" "${PROJECT_BINARY_DIR}/FlowieConfigVersion.cmake"
        DESTINATION lib/cmake/Flowie
)
install(
    EXPORT FlowieTargets
    FILE FlowieTargets.cmake
    NAMESPACE Flowie::
    DESTINATION lib/cmake/Flowie
)
export(
    EXPORT FlowieTargets
    FILE "${PROJECT_BINARY_DIR}/FlowieTargets.cmake"
    NAMESPACE Flowie::
)
