// PR-10: writing the results of a 60 s, 30 FPS run takes under 2 s. A timing test: release
// preset only (label `timing`, D9).
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>

#include "frameflow/config.hpp"
#include "frameflow/metrics/csv_reporter.hpp"
#include "frameflow/metrics/json_reporter.hpp"
#include "frameflow/metrics/run_report.hpp"
#include "frameflow/platform/environment.hpp"

namespace frameflow {
namespace {

namespace fs = std::filesystem;
using std::chrono::microseconds;
using std::chrono::milliseconds;

// 1800 frames with every timestamp set, a depth sample at each push and pop, some drop events.
RunReport sixty_seconds_at_30_fps() {
  RunReport report;
  report.t0 = Clock::time_point{} + std::chrono::seconds(1);
  report.t_end = report.t0 + std::chrono::seconds(60);
  for (std::uint64_t id = 0; id < 1800; ++id) {
    FrameMeta meta;
    meta.id = id;
    const Clock::time_point t = report.t0 + microseconds(33333 * static_cast<long long>(id));
    meta.t_cap = t;
    meta.t_push = t + microseconds(10);
    meta.t_pop = t + milliseconds(5);
    meta.t_proc_start = t + milliseconds(5);
    meta.t_proc_end = t + milliseconds(25);
    meta.t_out = t + milliseconds(26);
    report.frames.push_back(meta);
    report.depth.push_back({*meta.t_push, 1});
    report.depth.push_back({*meta.t_pop, 0});
    if (id % 10 == 0) report.events.push_back({t, "drop", id, "oldest"});
  }
  return report;
}

TEST(TimingResultWritingTest, SixtySecondsAt30FpsWrittenUnderTwoSeconds) {
  const fs::path dir = fs::temp_directory_path() / ("frameflow_pr10_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const RunReport report = sixty_seconds_at_30_fps();
  const Environment env = collect_environment();

  const auto start = std::chrono::steady_clock::now();
  write_csv_files(dir, report);
  write_summary_json(dir, make_summary_json(RunConfig{}, report, env, {}));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::seconds(2));
  EXPECT_GT(fs::file_size(dir / "frames.csv"), 1800u * 50u);
  fs::remove_all(dir);
}

}  // namespace
}  // namespace frameflow
