add_executable(frameflow_tests
  unit/adaptive_policy_test.cpp
  integration/bench_test.cpp
  integration/invariants_test.cpp
  integration/overload_test.cpp
  integration/pipeline_test.cpp
  integration/result_writing_test.cpp
  integration/run_command_test.cpp
  integration/signal_test.cpp
  integration/source_lateness_test.cpp
  stress/lifecycle_test.cpp
  stress/policy_stress_test.cpp
  unit/blocking_policy_test.cpp
  unit/bounded_queue_test.cpp
  unit/cli_test.cpp
  unit/clock_test.cpp
  unit/config_file_test.cpp
  unit/config_test.cpp
  unit/console_reporter_test.cpp
  unit/cost_model_test.cpp
  unit/csv_reporter_test.cpp
  unit/drop_policies_test.cpp
  unit/environment_test.cpp
  unit/experiment_test.cpp
  unit/frame_test.cpp
  unit/json_reporter_test.cpp
  unit/latency_window_test.cpp
  unit/metrics_store_test.cpp
  unit/opencv_smoke_test.cpp
  unit/output_dir_test.cpp
  unit/policy_factory_test.cpp
  unit/preview_sink_test.cpp
  unit/privileges_test.cpp
  unit/processor_test.cpp
  unit/progress_test.cpp
  unit/run_report_test.cpp
  unit/signal_watcher_test.cpp
  unit/source_runner_test.cpp
  unit/stats_test.cpp
  unit/synthetic_source_test.cpp
  unit/version_test.cpp
  unit/video_file_source_test.cpp
  unit/warnings_test.cpp
  unit/worker_test.cpp)

target_link_libraries(frameflow_tests PRIVATE
  frameflow_core
  frameflow_warnings
  opencv_core
  opencv_imgproc
  opencv_videoio
  opencv_highgui
  GTest::gtest_main)

target_compile_definitions(frameflow_tests PRIVATE
  FRAMEFLOW_EXPECTED_VERSION="${PROJECT_VERSION}"
  FRAMEFLOW_BINARY="$<TARGET_FILE:frameflow>"
  FRAMEFLOW_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_dependencies(frameflow_tests frameflow)  # signal tests spawn the real binary

# Test suites are labelled by name prefix (D9, D10): Timing* -> `timing` (release preset only),
# Long* -> `long`, Stress* -> `stress` (both only in the *-long presets). Everything else is
# unlabelled and runs in every default preset.
include(GoogleTest)
gtest_discover_tests(frameflow_tests TEST_FILTER "-Timing*:Long*:Stress*")
gtest_discover_tests(frameflow_tests TEST_FILTER "Timing*" PROPERTIES LABELS timing)
gtest_discover_tests(frameflow_tests TEST_FILTER "Long*" PROPERTIES LABELS long)
gtest_discover_tests(frameflow_tests TEST_FILTER "Stress*" PROPERTIES LABELS stress)

# P3.5: scripts/run_all.sh parses its options and finds every experiment file (prints only).
add_test(NAME RunAllDryRun
         COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/run_all.sh --dry-run
                 --binary $<TARGET_FILE:frameflow>)

# P5.9a: scripts/measure_perf.sh parses its options and finds every measurement file.
add_test(NAME MeasurePerfDryRun
         COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/measure_perf.sh --dry-run
                 --binary-dir $<TARGET_FILE_DIR:frameflow>)

# P3.6/P3.7: analyze.py tests (pytest, D7). The real-output test spawns the frameflow binary,
# except under TSan, whose binaries need `setarch -R` (see docs/DESIGN.md).
set(analyze_env "PYTHONDONTWRITEBYTECODE=1")
if(NOT FRAMEFLOW_SANITIZER STREQUAL "tsan")
  list(APPEND analyze_env "FRAMEFLOW_BINARY=$<TARGET_FILE:frameflow>")
endif()
add_test(NAME AnalyzePytest
         COMMAND python3 -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/scripts/tests)
set_tests_properties(AnalyzePytest PROPERTIES ENVIRONMENT "${analyze_env}")
