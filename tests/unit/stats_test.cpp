#include "frameflow/metrics/stats.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <numeric>
#include <vector>

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

const Clock::time_point kT0 = Clock::time_point{} + seconds(1000);

std::vector<double> one_to(int n) {
  std::vector<double> values(static_cast<std::size_t>(n));
  std::iota(values.begin(), values.end(), 1.0);
  return values;
}

// --- percentile (FR-MET-03) ---

TEST(PercentileTest, NearestRankOnOneToHundred) {
  const std::vector<double> values = one_to(100);
  EXPECT_EQ(percentile(values, 95), 95.0);  // SPEC 15: 1..100 -> p95 = 95
  EXPECT_EQ(percentile(values, 50), 50.0);
  EXPECT_EQ(percentile(values, 99), 99.0);
  EXPECT_EQ(percentile(values, 100), 100.0);
}

TEST(PercentileTest, RoundsRankUp) {
  const std::vector<double> values = one_to(10);
  EXPECT_EQ(percentile(values, 95), 10.0);  // ceil(9.5) = 10
  EXPECT_EQ(percentile(values, 50), 5.0);   // ceil(5.0) = 5
  EXPECT_EQ(percentile(values, 51), 6.0);   // ceil(5.1) = 6
}

TEST(PercentileTest, IgnoresInputOrder) { EXPECT_EQ(percentile({30, 10, 20}, 50), 20.0); }

TEST(PercentileTest, SingleValueIsEveryPercentile) {
  EXPECT_EQ(percentile({7.5}, 1), 7.5);
  EXPECT_EQ(percentile({7.5}, 100), 7.5);
}

TEST(PercentileTest, EmptyInputHasNoValue) { EXPECT_FALSE(percentile({}, 95).has_value()); }

// --- compute_run_stats ---

FrameMeta delivered(std::uint64_t id, Clock::duration t_cap, Clock::duration latency) {
  FrameMeta meta;
  meta.id = id;
  meta.t_cap = kT0 + t_cap;
  meta.t_out = kT0 + t_cap + latency;
  return meta;
}

FrameMeta dropped(std::uint64_t id, Clock::duration t_cap, DropReason reason) {
  FrameMeta meta;
  meta.id = id;
  meta.t_cap = kT0 + t_cap;
  meta.drop_reason = reason;
  return meta;
}

FrameMeta in_flight(std::uint64_t id, Clock::duration t_cap) {
  FrameMeta meta;
  meta.id = id;
  meta.t_cap = kT0 + t_cap;
  return meta;
}

TEST(RunStatsTest, CountsWholeRunAndConservesFrames) {
  const std::vector<FrameMeta> frames = {delivered(0, milliseconds(0), milliseconds(5)),
                                         dropped(1, milliseconds(10), DropReason::Oldest),
                                         in_flight(2, milliseconds(20))};
  const RunStats stats = compute_run_stats(frames, {}, kT0, kT0 + seconds(1), milliseconds(0));
  EXPECT_EQ(stats.produced, 3u);
  EXPECT_EQ(stats.delivered, 1u);
  EXPECT_EQ(stats.dropped, 1u);
  EXPECT_EQ(stats.in_flight, 1u);
}

TEST(RunStatsTest, LatencyPercentilesUseDeliveredFramesOnly) {
  std::vector<FrameMeta> frames;
  for (int i = 1; i <= 100; ++i) {
    frames.push_back(delivered(static_cast<std::uint64_t>(i), milliseconds(i), milliseconds(i)));
  }
  frames.push_back(dropped(101, milliseconds(200), DropReason::Newest));  // BR-05: ignored
  const RunStats stats = compute_run_stats(frames, {}, kT0, kT0 + seconds(1), milliseconds(0));
  EXPECT_DOUBLE_EQ(*stats.p50_ms, 50.0);
  EXPECT_DOUBLE_EQ(*stats.p95_ms, 95.0);
  EXPECT_DOUBLE_EQ(*stats.p99_ms, 99.0);
  EXPECT_DOUBLE_EQ(*stats.max_ms, 100.0);
}

// BR-06: frames captured during warm-up count in the totals but not in rates or percentiles.
TEST(RunStatsTest, ExcludesWarmupFromRatesAndPercentiles) {
  const std::vector<FrameMeta> frames = {
      delivered(0, milliseconds(100), milliseconds(900)),  // warm-up: huge latency, ignored
      dropped(1, milliseconds(500), DropReason::Oldest),   // warm-up drop, ignored in rate
      delivered(2, milliseconds(1000), milliseconds(10)),  // first frame of the window
      delivered(3, milliseconds(1500), milliseconds(20)),
  };
  const RunStats stats = compute_run_stats(frames, {}, kT0, kT0 + seconds(3), seconds(1));
  EXPECT_EQ(stats.produced, 4u);
  EXPECT_EQ(stats.dropped, 1u);
  EXPECT_EQ(stats.window_produced, 2u);
  EXPECT_DOUBLE_EQ(*stats.max_ms, 20.0);
  EXPECT_DOUBLE_EQ(*stats.drop_rate_pct, 0.0);
  EXPECT_DOUBLE_EQ(stats.window_s, 2.0);
  EXPECT_DOUBLE_EQ(*stats.input_fps, 1.0);  // 2 frames in 2 s
}

TEST(RunStatsTest, ThroughputAndDropRatesByReason) {
  std::vector<FrameMeta> frames;
  for (std::uint64_t id = 0; id < 10; ++id) {
    const Clock::duration t_cap = milliseconds(100 * static_cast<int>(id));
    if (id < 6) {
      frames.push_back(delivered(id, t_cap, milliseconds(1)));
    } else if (id < 9) {
      frames.push_back(dropped(id, t_cap, DropReason::Oldest));
    } else {
      frames.push_back(dropped(id, t_cap, DropReason::Newest));
    }
  }
  const RunStats stats = compute_run_stats(frames, {}, kT0, kT0 + seconds(1), milliseconds(0));
  EXPECT_DOUBLE_EQ(*stats.input_fps, 10.0);
  EXPECT_DOUBLE_EQ(*stats.output_fps, 6.0);
  EXPECT_DOUBLE_EQ(*stats.drop_rate_pct, 40.0);
  EXPECT_DOUBLE_EQ(*stats.drop_oldest_pct, 30.0);
  EXPECT_DOUBLE_EQ(*stats.drop_newest_pct, 10.0);
  EXPECT_DOUBLE_EQ(*stats.drop_stale_pct, 0.0);
}

TEST(RunStatsTest, DepthMeanAndMaxAfterWarmup) {
  const std::vector<DepthSample> depth = {
      {kT0 + milliseconds(10), 8},  // warm-up, ignored
      {kT0 + seconds(1), 1},
      {kT0 + seconds(1) + milliseconds(5), 3},
      {kT0 + seconds(2), 2},
  };
  const RunStats stats = compute_run_stats({}, depth, kT0, kT0 + seconds(3), seconds(1));
  EXPECT_DOUBLE_EQ(*stats.depth_mean, 2.0);
  EXPECT_EQ(*stats.depth_max, 3u);
}

// EC-15: every frame dropped: no latency values, 100% drop rate, no crash.
TEST(RunStatsTest, AllDroppedHasNoLatencyAndFullDropRate) {
  const std::vector<FrameMeta> frames = {dropped(0, milliseconds(0), DropReason::Newest),
                                         dropped(1, milliseconds(10), DropReason::Newest)};
  const RunStats stats = compute_run_stats(frames, {}, kT0, kT0 + seconds(1), milliseconds(0));
  EXPECT_FALSE(stats.p50_ms.has_value());
  EXPECT_FALSE(stats.max_ms.has_value());
  EXPECT_DOUBLE_EQ(*stats.drop_rate_pct, 100.0);
  EXPECT_DOUBLE_EQ(*stats.output_fps, 0.0);
}

// Stopped during warm-up (EC-08): empty window, so nothing can be computed.
TEST(RunStatsTest, EmptyWindowHasNoRates) {
  const std::vector<FrameMeta> frames = {delivered(0, milliseconds(0), milliseconds(5))};
  const RunStats stats = compute_run_stats(frames, {}, kT0, kT0 + milliseconds(500), seconds(3));
  EXPECT_EQ(stats.produced, 1u);
  EXPECT_DOUBLE_EQ(stats.window_s, 0.0);
  EXPECT_FALSE(stats.input_fps.has_value());
  EXPECT_FALSE(stats.drop_rate_pct.has_value());
  EXPECT_FALSE(stats.p95_ms.has_value());
  EXPECT_FALSE(stats.depth_mean.has_value());
}

}  // namespace
}  // namespace frameflow
