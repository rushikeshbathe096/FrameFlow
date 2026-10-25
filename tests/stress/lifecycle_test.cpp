// RR-02: 200 create-start-stop-destroy cycles in one process without deadlock, crash or leak
// (leaks are caught by ASan; races by TSan in the tsan-long preset). Label `stress` (D10).
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "../integration/invariants.hpp"
#include "frameflow/pipeline.hpp"
#include "frameflow/sink/null_sink.hpp"
#include "frameflow/source/synthetic_source.hpp"

namespace frameflow {
namespace {

constexpr int kCycles = 200;

std::unique_ptr<Pipeline> make_pipeline(Clock& clock, const PipelineConfig& config) {
  return std::make_unique<Pipeline>(config, clock,
                                    std::make_unique<SyntheticSource>(cv::Size(64, 48)),
                                    std::make_unique<NullSink>(clock));
}

TEST(StressLifecycleTest, RunToFrameCountRepeatedly) {
  SteadyClock clock;
  PipelineConfig config;
  config.fps = 240.0;
  config.frames = 3;
  for (int i = 0; i < kCycles; ++i) {
    std::unique_ptr<Pipeline> pipeline = make_pipeline(clock, config);
    const RunResult result = pipeline->run();
    ASSERT_EQ(result.ended_by, EndedBy::Frames) << "cycle " << i;
    testing::expect_run_invariants(pipeline->metrics(), result.produced);
  }
}

TEST(StressLifecycleTest, StopFromAnotherThreadRepeatedly) {
  SteadyClock clock;
  PipelineConfig config;
  config.fps = 240.0;
  config.duration = std::chrono::seconds(10);
  config.cost = std::chrono::milliseconds(2);
  config.queue_capacity = 1;  // the source is often blocked in push when the stop arrives
  for (int i = 0; i < kCycles; ++i) {
    std::unique_ptr<Pipeline> pipeline = make_pipeline(clock, config);
    std::thread stopper([&] {
      std::this_thread::sleep_for(std::chrono::microseconds(500 * (i % 10)));
      pipeline->request_stop();
    });
    const RunResult result = pipeline->run();
    stopper.join();
    ASSERT_EQ(result.ended_by, EndedBy::StopRequested) << "cycle " << i;
    testing::expect_run_invariants(pipeline->metrics(), result.produced);
  }
}

TEST(StressLifecycleTest, CreateAndDestroyWithoutRun) {
  SteadyClock clock;
  PipelineConfig config;
  config.frames = 10;
  for (int i = 0; i < kCycles; ++i) make_pipeline(clock, config);
}

}  // namespace
}  // namespace frameflow
