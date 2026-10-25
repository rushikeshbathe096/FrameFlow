#include "frameflow/metrics/warnings.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

// A valid 60 s run of `count` frames, none late.
RunReport long_run(std::size_t count) {
  RunReport report;
  report.t0 = Clock::time_point{} + seconds(100);
  report.t_end = report.t0 + seconds(60);
  report.ended_by = EndedBy::Duration;
  report.valid = true;
  report.frames.resize(count);
  return report;
}

RunConfig sixty_seconds() {
  RunConfig config;  // 60 s, warm-up 3 s
  return config;
}

bool has(const std::vector<std::string>& warnings, const std::string& part) {
  for (const std::string& w : warnings) {
    if (w.find(part) != std::string::npos) return true;
  }
  return false;
}

TEST(WarningsTest, HealthyRunHasNone) {
  EXPECT_TRUE(collect_warnings(sixty_seconds(), long_run(100)).empty());
}

TEST(WarningsTest, LateSourceAboveOnePercentWarns) {
  RunReport report = long_run(100);
  report.frames[0].source_lateness = milliseconds(6);
  report.frames[1].source_lateness = milliseconds(9);
  const auto warnings = collect_warnings(sixty_seconds(), report);
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_EQ(warnings[0], "source was late by > 5 ms on 2.0% of frames; system may be overloaded");
}

TEST(WarningsTest, LateSourceAtExactlyOnePercentOrFiveMsDoesNotWarn) {
  RunReport report = long_run(100);
  report.frames[0].source_lateness = milliseconds(50);  // 1 of 100 = 1%: not more than 1%
  report.frames[1].source_lateness = milliseconds(5);   // exactly 5 ms: not more than 5 ms
  EXPECT_FALSE(has(collect_warnings(sixty_seconds(), report), "source was late"));
}

// D38: lateness caused by waiting on a full queue (blocking backpressure) is not CPU overload.
TEST(WarningsTest, LatenessFromBackpressureDoesNotWarn) {
  RunReport report = long_run(100);
  for (FrameMeta& meta : report.frames) {
    meta.source_lateness = milliseconds(400);
    meta.backpressure = milliseconds(398);  // own lateness 2 ms
  }
  EXPECT_FALSE(has(collect_warnings(sixty_seconds(), report), "source was late"));
}

TEST(WarningsTest, OwnLatenessOnTopOfBackpressureStillWarns) {
  RunReport report = long_run(100);
  for (FrameMeta& meta : report.frames) {
    meta.source_lateness = milliseconds(400);
    meta.backpressure = milliseconds(390);  // own lateness 10 ms
  }
  EXPECT_TRUE(has(collect_warnings(sixty_seconds(), report), "on 100.0% of frames"));
}

TEST(WarningsTest, StoppedDuringWarmupWarns) {
  RunReport report = long_run(10);
  report.ended_by = EndedBy::StopRequested;
  report.valid = false;
  report.t_end = report.t0 + seconds(1);
  const auto warnings = collect_warnings(sixty_seconds(), report);
  EXPECT_TRUE(has(warnings, "stopped during warm-up; results marked invalid"));
  EXPECT_TRUE(has(warnings, "run shorter than 10 s"));
}

TEST(WarningsTest, ErrorRunIsNotReportedAsWarmupStop) {
  RunReport report = long_run(10);
  report.ended_by = EndedBy::Error;
  report.valid = false;
  EXPECT_FALSE(has(collect_warnings(sixty_seconds(), report), "warm-up"));
}

TEST(WarningsTest, WarmupLongerThanHalfTheRunWarns) {
  RunConfig config = sixty_seconds();
  config.warmup_s = 31.0;
  EXPECT_TRUE(has(collect_warnings(config, long_run(10)),
                  "warm-up (31 s) is longer than half the run (60 s)"));
  config.warmup_s = 30.0;
  EXPECT_FALSE(has(collect_warnings(config, long_run(10)), "half the run"));
}

TEST(WarningsTest, ShortRunWarns) {
  RunReport report = long_run(10);
  report.t_end = report.t0 + milliseconds(9999);
  EXPECT_TRUE(has(collect_warnings(sixty_seconds(), report),
                  "run shorter than 10 s; percentiles may be unstable"));
  report.t_end = report.t0 + seconds(10);
  EXPECT_FALSE(has(collect_warnings(sixty_seconds(), report), "shorter than 10 s"));
}

}  // namespace
}  // namespace frameflow
