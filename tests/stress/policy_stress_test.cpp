// SR-04 (stress part): 60 s of overloaded runs, 20 s per policy, with a one-frame queue so the
// source and worker contend on the queue lock on almost every frame. Run under TSan in the
// tsan-long preset; BR-01..BR-04 must hold for each run. Label `stress` (D10, D37).
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#include "../integration/invariants.hpp"
#include "frameflow/pipeline.hpp"
#include "frameflow/sink/null_sink.hpp"
#include "frameflow/source/synthetic_source.hpp"

namespace frameflow {
namespace {

class StressPolicyTest : public ::testing::TestWithParam<std::string> {};

TEST_P(StressPolicyTest, TwentySecondsAtTwiceOverloadWithQueueOfOne) {
  SteadyClock clock;
  PipelineConfig config;
  config.fps = 250.0;                          // 4 ms interval
  config.cost = std::chrono::milliseconds(8);  // 2.0x
  config.queue_capacity = 1;
  config.duration = std::chrono::seconds(20);
  config.policy = GetParam();
  Pipeline pipeline(config, clock, std::make_unique<SyntheticSource>(cv::Size(64, 48)),
                    std::make_unique<NullSink>(clock));
  const RunResult result = pipeline.run();
  EXPECT_EQ(result.ended_by, EndedBy::Duration);
  const testing::FrameCounts counts =
      testing::expect_run_invariants(pipeline.metrics(), result.produced);
  EXPECT_GT(counts.delivered, 0u);
  if (GetParam() == "blocking") {
    EXPECT_EQ(counts.dropped, 0u);
  } else {
    EXPECT_GT(counts.dropped, 0u);
  }
}

INSTANTIATE_TEST_SUITE_P(StressPolicy, StressPolicyTest,
                         ::testing::Values("blocking", "drop-oldest", "drop-newest"),
                         [](const auto& info) {
                           std::string name = info.param;
                           name.erase(std::remove(name.begin(), name.end(), '-'), name.end());
                           return name;
                         });

}  // namespace
}  // namespace frameflow
