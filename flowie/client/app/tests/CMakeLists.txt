file(GLOB _workbench_tests CONFIGURE_DEPENDS *.cpp *.h)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${_workbench_tests})
cmake_add_test(
        test_flowie_workbench SOURCES ${_workbench_tests} ../src/workbench.rc LIBS
        flowie_workbench_ui Flowie::Protocol Salts::TinyTest OpenWTL::WTL ws2_32 FOLDER
        "flowie/client/app/tests"
)
target_compile_definitions(test_flowie_workbench PRIVATE NOMINMAX)
if(MSVC)
        target_link_options(test_flowie_workbench PRIVATE /MANIFEST:NO)
endif()
set_tests_properties(
        test_flowie_workbench PROPERTIES LABELS "flowie;mqtt;client;workbench;lifetime" TIMEOUT 90
)
