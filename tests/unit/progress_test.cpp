#include "frameflow/ui/progress.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

const ProgressFrame kFrame{seconds(60), seconds(3), 8};

Pipeline::LiveStatus status(Clock::duration elapsed, std::uint64_t delivered, std::uint64_t dropped,
                            std::size_t depth, std::optional<Clock::duration> age,
                            bool shedding = false) {
  Pipeline::LiveStatus s;
  s.elapsed = elapsed;
  s.counts.delivered = delivered;
  s.counts.dropped = dropped;
  s.counts.depth = depth;
  s.counts.last_latency = age;
  s.shedding = shedding;
  return s;
}

// UIUX S3, with the values of its example.
TEST(ProgressTest, GoldenLine) {
  EXPECT_EQ(format_progress(status(milliseconds(24'400), 402, 198, 8, milliseconds(96), true),
                            kFrame, 19.8),
            "[####------]  24/60 s  ·  out 19.8 fps  ·  queue 8/8  ·  drops 33%  ·  age 96 ms  "
            "·  SHEDDING");
}

// UIUX 9.1: during warm-up the badge replaces the metrics.
TEST(ProgressTest, WarmupBadge) {
  EXPECT_EQ(format_progress(status(seconds(2), 50, 0, 1, milliseconds(5)), kFrame, 25.0),
            "[----------]  2/60 s  ·  warm-up");
}

TEST(ProgressTest, NothingDeliveredYet) {
  EXPECT_EQ(format_progress(status(seconds(5), 0, 0, 8, std::nullopt), kFrame, std::nullopt),
            "[----------]  5/60 s  ·  out - fps  ·  queue 8/8  ·  drops 0%  ·  age - ms");
}

TEST(ProgressTest, BarIsFullAtTheEnd) {
  const std::string line =
      format_progress(status(seconds(61), 10, 0, 0, milliseconds(1)), kFrame, 1.0);
  EXPECT_EQ(line.rfind("[##########]  61/60 s", 0), 0u) << line;
}

TEST(ProgressTest, OutputFpsIsTheDeliveryRateBetweenTicks) {
  ProgressLine line(kFrame, false);
  EXPECT_NE(line.update(status(seconds(4), 100, 0, 2, milliseconds(30))).find("out - fps"),
            std::string::npos);  // first tick: no rate yet
  EXPECT_NE(line.update(status(seconds(5), 120, 0, 2, milliseconds(30))).find("out 20.0 fps"),
            std::string::npos);
}

TEST(ProgressTest, TerminalRewritesTheLineInPlace) {
  ProgressLine line(kFrame, true);
  EXPECT_EQ(line.finish(), "");  // nothing printed yet
  const std::string text = line.update(status(seconds(4), 1, 0, 0, milliseconds(1)));
  EXPECT_EQ(text.rfind("\r\033[K[", 0), 0u);
  EXPECT_EQ(text.find('\n'), std::string::npos);
  EXPECT_EQ(line.finish(), "\n");
}

TEST(ProgressTest, PipeGetsOneLinePerTick) {
  ProgressLine line(kFrame, false);
  const std::string text = line.update(status(seconds(4), 1, 0, 0, milliseconds(1)));
  EXPECT_EQ(text.back(), '\n');
  EXPECT_EQ(text.find('\r'), std::string::npos);
  EXPECT_EQ(line.finish(), "");
}

}  // namespace
}  // namespace frameflow
