cmake_minimum_required(VERSION 3.28)

project(frameflow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FRAMEFLOW_HARDENING "Enable hardening flags (release)" OFF)
set(FRAMEFLOW_SANITIZER "" CACHE STRING "Sanitizer build: '', 'asan' or 'tsan'")
set_property(CACHE FRAMEFLOW_SANITIZER PROPERTY STRINGS "" asan tsan)

# Warnings are applied per target (not globally) so third-party headers are not affected.
add_library(frameflow_warnings INTERFACE)
target_compile_options(frameflow_warnings INTERFACE -Wall -Wextra -Werror)

# Sanitizers must be on both compile and link lines of every target, so they are global.
if(FRAMEFLOW_SANITIZER STREQUAL "asan")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(FRAMEFLOW_SANITIZER STREQUAL "tsan")
  # The tsan preset runs tests via `setarch -R` (CMAKE_CROSSCOMPILING_EMULATOR): GCC 13's
  # TSan runtime crashes at startup under the high ASLR entropy of recent kernels.
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT FRAMEFLOW_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown FRAMEFLOW_SANITIZER '${FRAMEFLOW_SANITIZER}'")
endif()

if(FRAMEFLOW_HARDENING)
  # -U first: Ubuntu's gcc predefines _FORTIFY_SOURCE, and redefining it warns (-Werror).
  add_compile_options(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fstack-protector-strong)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  add_link_options(-pie)
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(CLI11 2.4 REQUIRED)

# Git commit for `--version` and summary.json (FR-CLI-04, BR-10). Taken at configure time, so it
# is stale until CMake re-runs after a new commit (D23). Running git here is CMake, not C++ (SR-09).
find_package(Git QUIET)
set(FRAMEFLOW_GIT_COMMIT "unknown")
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                  OUTPUT_VARIABLE git_commit_out OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE git_commit_result ERROR_QUIET)
  if(git_commit_result EQUAL 0 AND git_commit_out)
    set(FRAMEFLOW_GIT_COMMIT "${git_commit_out}")
  endif()
endif()

# Generated header lives in the build tree so the version has one source: project() above.
configure_file(include/frameflow/version.hpp.in
               ${PROJECT_BINARY_DIR}/generated/frameflow/version.hpp @ONLY)

add_library(frameflow_core STATIC
  src/cli.cpp
  src/clock.cpp
  src/config.cpp
  src/pipeline.cpp
  src/platform/environment.cpp
  src/platform/output_dir.cpp
  src/platform/privileges.cpp
  src/platform/signals.cpp
  src/policy/policy_factory.cpp
  src/metrics/csv_reporter.cpp
  src/metrics/json_reporter.cpp
  src/metrics/metrics_store.cpp
  src/metrics/run_report.cpp
  src/metrics/stats.cpp
  src/metrics/warnings.cpp
  src/processor/cost_model.cpp
  src/run_command.cpp
  src/source/source_runner.cpp
  src/source/synthetic_source.cpp
  src/ui/console_reporter.cpp
  src/version.cpp
  src/worker.cpp)
target_include_directories(frameflow_core
  PUBLIC ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(frameflow_core
  PUBLIC Threads::Threads opencv_core nlohmann_json::nlohmann_json
  PRIVATE frameflow_warnings CLI11::CLI11)

add_executable(frameflow src/main.cpp)
target_link_libraries(frameflow PRIVATE frameflow_core frameflow_warnings)

enable_testing()
add_subdirectory(tests)
