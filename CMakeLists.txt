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

# Generated header lives in the build tree so the version has one source: project() above.
configure_file(include/frameflow/version.hpp.in
               ${PROJECT_BINARY_DIR}/generated/frameflow/version.hpp @ONLY)

add_library(frameflow_core STATIC
  src/clock.cpp
  src/config.cpp
  src/pipeline.cpp
  src/policy/policy_factory.cpp
  src/metrics/csv_reporter.cpp
  src/metrics/metrics_store.cpp
  src/metrics/run_report.cpp
  src/metrics/stats.cpp
  src/processor/cost_model.cpp
  src/source/source_runner.cpp
  src/source/synthetic_source.cpp
  src/version.cpp
  src/worker.cpp)
target_include_directories(frameflow_core
  PUBLIC ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(frameflow_core
  PUBLIC Threads::Threads opencv_core
  PRIVATE frameflow_warnings)

add_executable(frameflow src/main.cpp)
target_link_libraries(frameflow PRIVATE frameflow_core frameflow_warnings)

enable_testing()
add_subdirectory(tests)
