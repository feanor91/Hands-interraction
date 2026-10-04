add_executable(core_tests
  test_main.cpp
  test_util.cpp
  test_ini.cpp
  test_config.cpp
  test_logger.cpp)
target_link_libraries(core_tests PRIVATE hands_core)
add_test(NAME core_tests COMMAND core_tests)
