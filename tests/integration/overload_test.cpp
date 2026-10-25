// Phase 2 overload behaviour per policy (EC-03, BR-01..BR-04 for every policy). Checks are
// invariants and wide ranges, never exact timings (SPEC 15): blocking latency has a hard lower
// bound that grows with every frame, dropping policies drop about the excess and keep latency
// far below blocking.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "frameflow/metrics/run_report.hpp"
#include "frameflow/pipeline.hpp"
#include "frameflow/sink/null_sink.hpp"
#include "frameflow/source/synthetic_source.hpp"
#include "invariants.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;

constexpr milliseconds kInterval{20};  // 50 fps
constexpr milliseconds kRunLength{2000};
constexpr milliseconds kWarmup{500};
constexpr std::size_t kQueue = 4;

struct OverloadRun {
  RunReport report;
  testing::FrameCounts counts;
};

OverloadRun run_overloaded(const std::string& policy, milliseconds cost) {
  SteadyClock clock;
  PipelineConfig config;
  config.fps = 1000.0 / static_cast<double>(kInterval.count());
  config.duration = kRunLength;
  config.cost = cost;
  config.queue_capacity = kQueue;
  config.policy = policy;
  Pipeline pipeline(config, clock, std::make_unique<SyntheticSource>(cv::Size(64, 48)),
                    std::make_unique<NullSink>(clock));
  const RunResult result = pipeline.run();
  EXPECT_EQ(result.ended_by, EndedBy::Duration);
  OverloadRun run;
  run.counts = testing::expect_run_invariants(pipeline.metrics(), result.produced);
  run.report = make_run_report(pipeline.metrics(), result, kWarmup);
  return run;
}

double latency_ms(const FrameMeta& meta) {
  return std::chrono::duration<double, std::milli>(*meta.t_out - *meta.t_cap).count();
}

// Blocking processes every frame in id order from t0, so frame k cannot be finished before
// t0 + (k + 1) x cost, while it was scheduled at t0 + k x interval: latency grows linearly.
void expect_blocking_latency_grows(const OverloadRun& run, milliseconds cost) {
  EXPECT_EQ(run.counts.dropped, 0u);
  ASSERT_GE(run.counts.delivered, 5u);  // 2 s at a 200 ms cost delivers about 10
  for (const FrameMeta& meta : run.report.frames) {
    if (!meta.t_out) continue;
    const double k = static_cast<double>(meta.id);
    const double bound = (k + 1.0) * static_cast<double>(cost.count()) -
                         k * static_cast<double>(kInterval.count()) - 1.0;  // 1 ms rounding
    EXPECT_GE(latency_ms(meta), bound) << "frame " << meta.id;
  }
}

// Mean latency of the delivered frames after warm-up, first third vs last third.
std::pair<double, double> first_and_last_third(const RunReport& report) {
  std::vector<double> latencies;
  for (const FrameMeta& meta : report.frames) {
    if (meta.t_out && !meta.warmup) latencies.push_back(latency_ms(meta));
  }
  const std::size_t third = latencies.size() / 3;
  double first = 0.0;
  double last = 0.0;
  for (std::size_t i = 0; i < third; ++i) {
    first += latencies[i];
    last += latencies[latencies.size() - 1 - i];
  }
  return {first / static_cast<double>(third), last / static_cast<double>(third)};
}

class DroppingPolicyTest : public ::testing::TestWithParam<std::string> {};

TEST_P(DroppingPolicyTest, DropsAboutHalfAtTwiceOverload) {
  const OverloadRun run = run_overloaded(GetParam(), milliseconds(40));  // 2.0x
  ASSERT_TRUE(run.report.stats.drop_rate_pct.has_value());
  EXPECT_GE(*run.report.stats.drop_rate_pct, 40.0);
  EXPECT_LE(*run.report.stats.drop_rate_pct, 60.0);
}

TEST_P(DroppingPolicyTest, LatencyStaysBoundedWhileBlockingGrows) {
  const OverloadRun dropping = run_overloaded(GetParam(), milliseconds(40));
  const OverloadRun blocking = run_overloaded("blocking", milliseconds(40));
  expect_blocking_latency_grows(blocking, milliseconds(40));
  ASSERT_TRUE(dropping.report.stats.max_ms && blocking.report.stats.max_ms);
  EXPECT_LT(*dropping.report.stats.max_ms, *blocking.report.stats.max_ms / 2.0);
  const auto [first, last] = first_and_last_third(dropping.report);
  EXPECT_LT(last, first * 1.5 + 20.0) << "dropping latency should not keep growing";
  const auto [blocking_first, blocking_last] = first_and_last_third(blocking.report);
  EXPECT_GT(blocking_last, blocking_first * 1.5) << "blocking latency should keep growing";
}

// EC-03: cost 10x the frame interval.
TEST_P(DroppingPolicyTest, ExtremeOverloadDropsAboutNinetyPercent) {
  const OverloadRun run = run_overloaded(GetParam(), milliseconds(200));
  ASSERT_TRUE(run.report.stats.drop_rate_pct.has_value());
  EXPECT_GE(*run.report.stats.drop_rate_pct, 80.0);
  EXPECT_LE(*run.report.stats.drop_rate_pct, 97.0);
  EXPECT_GT(run.counts.delivered, 0u);
}

INSTANTIATE_TEST_SUITE_P(OverloadTest, DroppingPolicyTest,
                         ::testing::Values("drop-oldest", "drop-newest"), [](const auto& info) {
                           return info.param == "drop-oldest" ? "DropOldest" : "DropNewest";
                         });

// P4.5: adaptive at 2.0x sheds (stale/oldest drops, at least one state change, logged as events)
// and keeps latency far below blocking; BR-01..BR-04 hold (checked in run_overloaded).
TEST(OverloadTest, AdaptiveShedsUnderOverloadAndKeepsLatencyBounded) {
  const OverloadRun adaptive = run_overloaded("adaptive", milliseconds(40));
  const OverloadRun blocking = run_overloaded("blocking", milliseconds(40));
  EXPECT_GT(adaptive.counts.dropped, 0u);
  for (const FrameMeta& meta : adaptive.report.frames) {
    if (!meta.is_dropped()) continue;
    EXPECT_TRUE(meta.drop_reason == DropReason::Stale || meta.drop_reason == DropReason::Oldest)
        << "frame " << meta.id;
    EXPECT_TRUE(meta.t_drop.has_value()) << "frame " << meta.id;
  }
  ASSERT_TRUE(adaptive.report.stats.state_changes.has_value());
  EXPECT_GE(*adaptive.report.stats.state_changes, 1u);
  ASSERT_TRUE(adaptive.report.stats.shedding_pct.has_value());
  EXPECT_GT(*adaptive.report.stats.shedding_pct, 0.0);
  const bool logged = std::any_of(adaptive.report.events.begin(), adaptive.report.events.end(),
                                  [](const RunEvent& e) { return e.type == "state_change"; });
  EXPECT_TRUE(logged);  // FR-POL-09 in events.csv
  ASSERT_TRUE(adaptive.report.stats.max_ms && blocking.report.stats.max_ms);
  EXPECT_LT(*adaptive.report.stats.max_ms, *blocking.report.stats.max_ms / 2.0);
  EXPECT_FALSE(blocking.report.stats.state_changes.has_value());  // fixed policy: no adaptive stats
}

// BR-07: without overload, adaptive never leaves NORMAL and never drops.
TEST(OverloadTest, AdaptiveWithoutOverloadNeverDrops) {
  const OverloadRun run = run_overloaded("adaptive", milliseconds(10));  // 0.5x
  EXPECT_EQ(run.counts.dropped, 0u);
  ASSERT_TRUE(run.report.stats.state_changes.has_value());
  EXPECT_EQ(*run.report.stats.state_changes, 0u);
  EXPECT_EQ(run.report.stats.shedding_pct, 0.0);
}

TEST(OverloadTest, BlockingLatencyGrowsLinearlyAtExtremeOverload) {
  const OverloadRun run = run_overloaded("blocking", milliseconds(200));  // EC-03
  expect_blocking_latency_grows(run, milliseconds(200));
}

}  // namespace
}  // namespace frameflow
