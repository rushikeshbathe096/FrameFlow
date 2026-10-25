#include "frameflow/metrics/run_report.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

const Clock::time_point kT0 = Clock::time_point{} + seconds(50);

FrameMeta captured(std::uint64_t id, Clock::duration t_cap) {
  FrameMeta meta;
  meta.id = id;
  meta.t_cap = kT0 + t_cap;
  return meta;
}

RunResult result_ending_at(Clock::duration t_end, EndedBy ended_by = EndedBy::Duration) {
  RunResult result;
  result.t0 = kT0;
  result.t_end = kT0 + t_end;
  result.ended_by = ended_by;
  return result;
}

TEST(RunReportTest, MarksWarmupFramesByCaptureTime) {
  MetricsStore metrics(3);
  metrics.record_frame(captured(0, milliseconds(0)));
  metrics.record_frame(captured(1, milliseconds(999)));
  metrics.record_frame(captured(2, milliseconds(1000)));  // first frame after warm-up
  const RunReport report = make_run_report(metrics, result_ending_at(seconds(5)), seconds(1));
  ASSERT_EQ(report.frames.size(), 3u);
  EXPECT_TRUE(report.frames[0].warmup);
  EXPECT_TRUE(report.frames[1].warmup);
  EXPECT_FALSE(report.frames[2].warmup);  // BR-06: kept in frames.csv, flagged only if inside
}

TEST(RunReportTest, ValidWhenWarmupCompleted) {
  MetricsStore metrics(1);
  EXPECT_TRUE(make_run_report(metrics, result_ending_at(seconds(5)), seconds(3)).valid);
  EXPECT_TRUE(make_run_report(metrics, result_ending_at(seconds(3)), seconds(3)).valid);
}

// BR-09 / EC-08: stopped before the warm-up ended -> invalid.
TEST(RunReportTest, InvalidWhenStoppedDuringWarmup) {
  MetricsStore metrics(1);
  const RunReport report = make_run_report(
      metrics, result_ending_at(milliseconds(2500), EndedBy::StopRequested), seconds(3));
  EXPECT_FALSE(report.valid);
}

TEST(RunReportTest, InvalidWhenRunEndedByError) {
  MetricsStore metrics(1);
  RunResult result = result_ending_at(seconds(10), EndedBy::Error);
  result.error = "worker: boom";
  const RunReport report = make_run_report(metrics, result, seconds(1));
  EXPECT_FALSE(report.valid);
  EXPECT_EQ(report.error, "worker: boom");
}

TEST(RunReportTest, DropEventsInTimeOrder) {
  MetricsStore metrics(3);
  FrameMeta late = captured(0, milliseconds(0));
  late.drop_reason = DropReason::Oldest;
  late.t_drop = kT0 + milliseconds(40);
  FrameMeta early = captured(1, milliseconds(10));
  early.drop_reason = DropReason::Newest;
  early.t_drop = kT0 + milliseconds(10);
  metrics.record_frame(late);
  metrics.record_frame(early);
  metrics.record_frame(captured(2, milliseconds(20)));  // not dropped: no event

  const RunReport report = make_run_report(metrics, result_ending_at(seconds(1)), seconds(0));
  ASSERT_EQ(report.events.size(), 2u);
  EXPECT_EQ(report.events[0].frame_id, 1u);
  EXPECT_EQ(report.events[0].detail, "newest");
  EXPECT_EQ(report.events[1].frame_id, 0u);
  EXPECT_EQ(report.events[1].type, "drop");
  EXPECT_EQ(report.events[1].detail, "oldest");
}

// FR-POL-09 / DR-03: adaptive state changes become events, in time order with the drops.
TEST(RunReportTest, StateChangesBecomeEvents) {
  MetricsStore metrics(2);
  FrameMeta dropped = captured(0, milliseconds(0));
  dropped.drop_reason = DropReason::Stale;
  dropped.t_drop = kT0 + milliseconds(1500);
  metrics.record_frame(dropped);
  RunResult result = result_ending_at(seconds(10));
  result.state_changes = std::vector<StateChange>{
      {kT0 + seconds(1), AdaptiveState::Normal, AdaptiveState::Shedding, milliseconds(123)},
      {kT0 + seconds(2), AdaptiveState::Shedding, AdaptiveState::Normal, milliseconds(65)}};
  const RunReport report = make_run_report(metrics, result, seconds(0));
  ASSERT_EQ(report.events.size(), 3u);
  EXPECT_EQ(report.events[0].type, "state_change");
  EXPECT_FALSE(report.events[0].frame_id.has_value());
  EXPECT_EQ(report.events[0].detail, "NORMAL->SHEDDING L_ms=123.000");
  EXPECT_EQ(report.events[1].type, "drop");
  EXPECT_EQ(report.events[2].detail, "SHEDDING->NORMAL L_ms=65.000");
  EXPECT_EQ(report.stats.state_changes, 2u);
}

// D45: share of the measurement window spent in SHEDDING; warm-up time does not count.
TEST(RunReportTest, SheddingShareOfTheWindow) {
  MetricsStore metrics(1);
  RunResult result = result_ending_at(seconds(10));
  result.state_changes = std::vector<StateChange>{
      {kT0 + seconds(1), AdaptiveState::Normal, AdaptiveState::Shedding, milliseconds(200)},
      {kT0 + seconds(4), AdaptiveState::Shedding, AdaptiveState::Normal, milliseconds(50)},
      {kT0 + seconds(8), AdaptiveState::Normal, AdaptiveState::Shedding, milliseconds(200)}};
  // Window [2 s, 10 s): shedding 2-4 s and 8-10 s = 4 s of 8 s.
  const RunReport report = make_run_report(metrics, result, seconds(2));
  ASSERT_TRUE(report.stats.shedding_pct.has_value());
  EXPECT_DOUBLE_EQ(*report.stats.shedding_pct, 50.0);
}

TEST(RunReportTest, FixedPolicyHasNoAdaptiveStats) {
  MetricsStore metrics(1);
  const RunReport report = make_run_report(metrics, result_ending_at(seconds(5)), seconds(1));
  EXPECT_FALSE(report.stats.state_changes.has_value());
  EXPECT_FALSE(report.stats.shedding_pct.has_value());
}

TEST(RunReportTest, CopiesDepthAndComputesStats) {
  MetricsStore metrics(1);
  FrameMeta meta = captured(0, milliseconds(0));
  meta.t_out = kT0 + milliseconds(7);
  metrics.record_frame(meta);
  metrics.record_depth(kT0, 1);
  const RunReport report = make_run_report(metrics, result_ending_at(seconds(1)), seconds(0));
  EXPECT_EQ(report.depth.size(), 1u);
  EXPECT_EQ(report.stats.delivered, 1u);
  EXPECT_DOUBLE_EQ(*report.stats.p50_ms, 7.0);
}

}  // namespace
}  // namespace frameflow
