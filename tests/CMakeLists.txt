add_executable(frameflow_tests
  integration/invariants_test.cpp
  integration/pipeline_test.cpp
  stress/lifecycle_test.cpp
  unit/blocking_policy_test.cpp
  unit/bounded_queue_test.cpp
  unit/clock_test.cpp
  unit/config_test.cpp
  unit/cost_model_test.cpp
  unit/csv_reporter_test.cpp
  unit/drop_policies_test.cpp
  unit/frame_test.cpp
  unit/metrics_store_test.cpp
  unit/opencv_smoke_test.cpp
  unit/policy_factory_test.cpp
  unit/run_report_test.cpp
  unit/source_runner_test.cpp
  unit/stats_test.cpp
  unit/synthetic_source_test.cpp
  unit/version_test.cpp
  unit/worker_test.cpp)

target_link_libraries(frameflow_tests PRIVATE
  frameflow_core
  frameflow_warnings
  opencv_core
  opencv_imgproc
  GTest::gtest_main)

target_compile_definitions(frameflow_tests PRIVATE
  FRAMEFLOW_EXPECTED_VERSION="${PROJECT_VERSION}")

# Test suites are labelled by name prefix (D9, D10): Timing* -> `timing` (release preset only),
# Long* -> `long`, Stress* -> `stress` (both only in the *-long presets). Everything else is
# unlabelled and runs in every default preset.
include(GoogleTest)
gtest_discover_tests(frameflow_tests TEST_FILTER "-Timing*:Long*:Stress*")
gtest_discover_tests(frameflow_tests TEST_FILTER "Timing*" PROPERTIES LABELS timing)
gtest_discover_tests(frameflow_tests TEST_FILTER "Long*" PROPERTIES LABELS long)
gtest_discover_tests(frameflow_tests TEST_FILTER "Stress*" PROPERTIES LABELS stress)
