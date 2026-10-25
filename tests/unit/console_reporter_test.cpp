#include "frameflow/ui/console_reporter.hpp"

#include <gtest/gtest.h>

#include <string>

namespace frameflow {
namespace {

RunConfig drop_oldest_config() {
  RunConfig config;
  config.policy = "drop-oldest";
  config.cost_ms = 50.0;
  config.warmup_s = 0.0;
  return config;
}

FrameMeta dropped(DropReason reason) {
  FrameMeta meta;
  meta.drop_reason = reason;
  return meta;
}

RunReport overloaded_report() {
  RunReport report;
  report.valid = true;
  report.frames = {FrameMeta{}, dropped(DropReason::Oldest), dropped(DropReason::Oldest),
                   dropped(DropReason::Newest)};
  RunStats& s = report.stats;
  s.produced = 61;
  s.delivered = 40;
  s.dropped = 13;
  s.in_flight = 8;
  s.p50_ms = 286.15;
  s.p95_ms = 302.85;
  s.p99_ms = 302.86;
  s.max_ms = 302.86;
  s.input_fps = 30.496;
  s.output_fps = 19.997;
  s.drop_rate_pct = 21.311;
  s.depth_mean = 6.2178;
  s.depth_max = 8;
  return report;
}

TEST(ConsoleReporterTest, GoldenSummary) {
  const std::string expected =
      "Result  ·  policy drop-oldest  ·  overload 1.50x  ·  valid\n"
      "\n"
      "  Latency (ms)     p50 286.1   p95 302.9   p99 302.9   max 302.9\n"
      "  Throughput       in 30.5 fps   out 20.0 fps\n"
      "  Frames           produced 61   delivered 40   dropped 13 (21.3%)   in-flight 8\n"
      "                   oldest 2   newest 1   stale 0\n"
      "  Queue depth      mean 6.2   peak 8 / 8\n";
  EXPECT_EQ(format_run_summary(drop_oldest_config(), overloaded_report()), expected);
}

TEST(ConsoleReporterTest, NoFramesDeliveredShowsEmptyState) {
  RunReport report;  // every optional statistic is empty
  report.valid = false;
  report.stats.produced = 5;
  report.stats.in_flight = 5;
  RunConfig config;
  config.warmup_s = 0.0;
  const std::string expected =
      "Result  ·  policy blocking  ·  overload 0.00x  ·  invalid\n"
      "\n"
      "  Latency (ms)     —  no frames delivered\n"
      "  Throughput       in — fps   out — fps\n"
      "  Frames           produced 5   delivered 0   dropped 0   in-flight 5\n"
      "  Queue depth      mean —   peak — / 8\n";
  EXPECT_EQ(format_run_summary(config, report), expected);
}

TEST(ConsoleReporterTest, NoDropsOmitsReasonBreakdown) {
  RunReport report = overloaded_report();
  report.frames = {FrameMeta{}};
  report.stats.dropped = 0;
  report.stats.drop_rate_pct = 0.0;
  const std::string text = format_run_summary(drop_oldest_config(), report);
  EXPECT_NE(text.find("dropped 0 (0.0%)"), std::string::npos) << text;
  EXPECT_EQ(text.find("   oldest "), std::string::npos) << text;  // breakdown row, not the policy
}

TEST(ConsoleReporterTest, WarmupNoteExplainsWhatIsExcluded) {
  RunConfig config = drop_oldest_config();
  config.warmup_s = 3.0;
  const std::string text = format_run_summary(config, overloaded_report());
  EXPECT_NE(text.find("(frame counts: whole run; latency, fps, drop % and depth: after the 3 s "
                      "warm-up)\n"),
            std::string::npos)
      << text;
}

TEST(ConsoleReporterTest, WarningsHavePrefixOnePerLine) {
  EXPECT_EQ(format_warnings({"a", "b c"}), "frameflow: warning: a\nframeflow: warning: b c\n");
  EXPECT_EQ(format_warnings({}), "");
}

}  // namespace
}  // namespace frameflow
