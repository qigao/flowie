cmake_add_test(
  test_flowie_core
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_flowie_core.c
  LIBS Flowie::App TurboUtils::TinyTest
  FOLDER "flowie/tests")

set_tests_properties(test_flowie_core PROPERTIES LABELS "flowie;flowie-release;mqtt-fast")
