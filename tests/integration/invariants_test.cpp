// Phase 1 integration runs with the real clock and full-size synthetic frames. Every run checks
// BR-01..BR-04 through expect_run_invariants().
#include "invariants.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "frameflow/pipeline.hpp"
#include "frameflow/sink/null_sink.hpp"
#include "frameflow/source/synthetic_source.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;
using testing::expect_run_invariants;
using testing::FrameCounts;

struct RunOutcome {
  RunResult result;
  FrameCounts counts;
};

RunOutcome run_synthetic(const PipelineConfig& config, cv::Size resolution = cv::Size(1280, 720)) {
  SteadyClock clock;
  Pipeline pipeline(config, clock, std::make_unique<SyntheticSource>(resolution),
                    std::make_unique<NullSink>(clock));
  RunOutcome run;
  run.result = pipeline.run();
  run.counts = expect_run_invariants(pipeline.metrics(), run.result.produced);
  return run;
}

PipelineConfig for_duration(Clock::duration duration, double fps) {
  PipelineConfig config;
  config.fps = fps;
  config.duration = duration;
  return config;
}

// EC-02: with zero processing cost every frame is delivered except at most the one in flight
// when the run ends, and nothing is dropped.
TEST(InvariantsTest, CostZeroDeliversEveryFrame) {
  const RunOutcome run = run_synthetic(for_duration(seconds(1), 30.0));
  EXPECT_EQ(run.result.ended_by, EndedBy::Duration);
  EXPECT_GE(run.result.produced, 29u);
  EXPECT_EQ(run.counts.dropped, 0u);
  EXPECT_LE(run.counts.in_flight, 1u);
  EXPECT_EQ(run.counts.delivered + run.counts.in_flight, run.result.produced);
}

// 0.5x load (10 ms cost at a 20 ms interval): the worker keeps up, so the queue never backs up.
TEST(InvariantsTest, HalfLoadKeepsUp) {
  PipelineConfig config = for_duration(seconds(1), 50.0);
  config.cost = milliseconds(10);
  const RunOutcome run = run_synthetic(config);
  EXPECT_EQ(run.counts.dropped, 0u);
  EXPECT_LE(run.counts.in_flight, 2u);
}

// 2x overload under the blocking baseline: the source blocks, but invariants still hold and
// blocking never drops (FR-POL-02).
TEST(InvariantsTest, OverloadUnderBlockingKeepsInvariants) {
  PipelineConfig config = for_duration(milliseconds(500), 100.0);
  config.cost = milliseconds(20);
  config.queue_capacity = 4;
  const RunOutcome run = run_synthetic(config, cv::Size(320, 240));
  EXPECT_EQ(run.counts.dropped, 0u);
  EXPECT_GT(run.counts.delivered, 0u);
}

TEST(InvariantsTest, FrameCountRunReleasesExactlyThatMany) {
  PipelineConfig config;
  config.fps = 120.0;
  config.frames = 60;
  const RunOutcome run = run_synthetic(config);
  EXPECT_EQ(run.result.ended_by, EndedBy::Frames);
  EXPECT_EQ(run.result.produced, 60u);
}

// Phase 1 gate (SPEC 16): 60 s at 30 FPS with no overload, zero drops. Label `long` (D3, D10).
TEST(LongPhaseOneGateTest, SixtySecondsAtThirtyFpsWithoutDrops) {
  const RunOutcome run = run_synthetic(for_duration(seconds(60), 30.0));
  EXPECT_EQ(run.result.ended_by, EndedBy::Duration);
  EXPECT_GE(run.result.produced, 1799u);
  EXPECT_LE(run.result.produced, 1801u);
  EXPECT_EQ(run.counts.dropped, 0u);
  EXPECT_LE(run.counts.in_flight, 1u);
}

}  // namespace
}  // namespace frameflow
