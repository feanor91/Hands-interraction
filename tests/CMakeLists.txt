add_executable(core_tests
  test_main.cpp
  test_util.cpp
  test_ini.cpp
  test_config.cpp
  test_iniedit.cpp
  test_pinch.cpp
  test_clap.cpp
  test_zonetracker.cpp
  test_handpresence.cpp
  test_simnames.cpp
  test_posemath.cpp
  test_reach.cpp
  test_servo_math.cpp
  test_servo_controller.cpp
  test_pointer_detect.cpp
  test_gain_probe.cpp
  test_servo_brain.cpp
  test_logger.cpp)
target_link_libraries(core_tests PRIVATE hands_core)
target_compile_definitions(core_tests PRIVATE HANDS_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME core_tests COMMAND core_tests)
