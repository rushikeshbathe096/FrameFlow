add_executable(frameflow_tests
  unit/version_test.cpp
  unit/opencv_smoke_test.cpp)

target_link_libraries(frameflow_tests PRIVATE
  frameflow_core
  frameflow_warnings
  opencv_core
  opencv_imgproc
  GTest::gtest_main)

target_compile_definitions(frameflow_tests PRIVATE
  FRAMEFLOW_EXPECTED_VERSION="${PROJECT_VERSION}")

include(GoogleTest)
gtest_discover_tests(frameflow_tests)
