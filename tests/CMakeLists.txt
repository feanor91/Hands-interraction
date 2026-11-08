add_executable(core_tests
  test_main.cpp
  test_util.cpp
  test_ini.cpp
  test_config.cpp
  test_iniedit.cpp
  test_pinch.cpp
  test_zonetracker.cpp
  test_handpresence.cpp
  test_simnames.cpp
  test_posemath.cpp
  test_logger.cpp)
target_link_libraries(core_tests PRIVATE hands_core)
add_test(NAME core_tests COMMAND core_tests)
