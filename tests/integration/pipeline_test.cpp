#include "frameflow/pipeline.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "frameflow/sink/null_sink.hpp"
#include "frameflow/source/synthetic_source.hpp"
#include "invariants.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using Steady = std::chrono::steady_clock;
using testing::expect_run_invariants;

const cv::Size kSmall(64, 48);

std::unique_ptr<ISource> small_source() { return std::make_unique<SyntheticSource>(kSmall); }

PipelineConfig frames_config(std::uint64_t frames, double fps = 200.0) {
  PipelineConfig config;
  config.fps = fps;
  config.frames = frames;
  return config;
}

PipelineConfig duration_config(Clock::duration duration, double fps = 100.0) {
  PipelineConfig config;
  config.fps = fps;
  config.duration = duration;
  return config;
}

class ThrowingSink final : public ISink {
 public:
  ThrowingSink(Clock& clock, int fail_at) : inner_(clock), fail_at_(fail_at) {}
  void consume(Frame& frame) override {
    if (++count_ == fail_at_) throw std::runtime_error("sink exploded");
    inner_.consume(frame);
  }

 private:
  NullSink inner_;
  int fail_at_;
  int count_ = 0;
};

class ThrowingSource final : public ISource {
 public:
  FramePtr next(std::uint64_t id) override {
    if (id == 5) throw std::runtime_error("decode failed");
    return inner_.next(id);
  }

 private:
  SyntheticSource inner_{kSmall};
};

class EndingSource final : public ISource {
 public:
  FramePtr next(std::uint64_t id) override { return id < 5 ? inner_.next(id) : nullptr; }

 private:
  SyntheticSource inner_{kSmall};
};

TEST(PipelineTest, EndsWhenInputEnds) {
  SteadyClock clock;
  Pipeline pipeline(duration_config(std::chrono::seconds(60)), clock,
                    std::make_unique<EndingSource>(), std::make_unique<NullSink>(clock));
  const auto start = Steady::now();
  const RunResult result = pipeline.run();
  EXPECT_LT(Steady::now() - start, std::chrono::seconds(2));  // does not wait for the duration
  EXPECT_EQ(result.ended_by, EndedBy::InputEnd);
  EXPECT_EQ(result.produced, 5u);
  expect_run_invariants(pipeline.metrics(), result.produced);
}

TEST(PipelineTest, RejectsInvalidConfig) {
  SteadyClock clock;
  auto make = [&](const PipelineConfig& config) {
    Pipeline(config, clock, small_source(), std::make_unique<NullSink>(clock));
  };
  PipelineConfig both = frames_config(10);
  both.duration = milliseconds(100);
  EXPECT_THROW(make(both), std::invalid_argument);
  EXPECT_THROW(make(PipelineConfig{}), std::invalid_argument);  // neither
  PipelineConfig zero_fps = frames_config(10);
  zero_fps.fps = 0;
  EXPECT_THROW(make(zero_fps), std::invalid_argument);
  PipelineConfig zero_queue = frames_config(10);
  zero_queue.queue_capacity = 0;
  EXPECT_THROW(make(zero_queue), std::invalid_argument);
  EXPECT_THROW(make(frames_config(0)), std::invalid_argument);
}

TEST(PipelineTest, EndsAfterFrameCount) {
  SteadyClock clock;
  Pipeline pipeline(frames_config(20), clock, small_source(), std::make_unique<NullSink>(clock));
  const RunResult result = pipeline.run();
  EXPECT_EQ(result.ended_by, EndedBy::Frames);
  EXPECT_EQ(result.produced, 20u);
  EXPECT_TRUE(result.error.empty());
  expect_run_invariants(pipeline.metrics(), result.produced);
}

TEST(PipelineTest, EndsAfterDuration) {
  SteadyClock clock;
  Pipeline pipeline(duration_config(milliseconds(300)), clock, small_source(),
                    std::make_unique<NullSink>(clock));
  const auto start = Steady::now();
  const RunResult result = pipeline.run();
  EXPECT_GE(Steady::now() - start, milliseconds(300));
  EXPECT_EQ(result.ended_by, EndedBy::Duration);
  // 100 FPS for 300 ms releases frames at 0, 10, ..., 290 ms (+1 if the end lands on 300 ms).
  EXPECT_GE(result.produced, 20u);
  EXPECT_LE(result.produced, 31u);
  expect_run_invariants(pipeline.metrics(), result.produced);
}

TEST(PipelineTest, RequestStopFromAnotherThreadEndsRunQuickly) {
  SteadyClock clock;
  Pipeline pipeline(duration_config(std::chrono::seconds(60)), clock, small_source(),
                    std::make_unique<NullSink>(clock));
  std::thread stopper([&] {
    std::this_thread::sleep_for(milliseconds(100));
    pipeline.request_stop();
  });
  const auto start = Steady::now();
  const RunResult result = pipeline.run();
  stopper.join();
  EXPECT_LT(Steady::now() - start, std::chrono::seconds(2));
  EXPECT_EQ(result.ended_by, EndedBy::StopRequested);
  expect_run_invariants(pipeline.metrics(), result.produced);
}

TEST(PipelineTest, StopRequestedBeforeRunReturnsImmediately) {
  SteadyClock clock;
  Pipeline pipeline(duration_config(std::chrono::seconds(60)), clock, small_source(),
                    std::make_unique<NullSink>(clock));
  pipeline.request_stop();
  const RunResult result = pipeline.run();
  EXPECT_EQ(result.ended_by, EndedBy::StopRequested);
  expect_run_invariants(pipeline.metrics(), result.produced);
}

// ER-04: an exception on the worker thread stops the whole pipeline; nothing hangs.
TEST(PipelineTest, WorkerExceptionStopsPipeline) {
  SteadyClock clock;
  Pipeline pipeline(duration_config(std::chrono::seconds(60)), clock, small_source(),
                    std::make_unique<ThrowingSink>(clock, 3));
  const RunResult result = pipeline.run();
  EXPECT_EQ(result.ended_by, EndedBy::Error);
  EXPECT_EQ(result.error, "worker: sink exploded");
  expect_run_invariants(pipeline.metrics(), result.produced);
}

TEST(PipelineTest, SourceExceptionStopsPipeline) {
  SteadyClock clock;
  Pipeline pipeline(duration_config(std::chrono::seconds(60)), clock,
                    std::make_unique<ThrowingSource>(), std::make_unique<NullSink>(clock));
  const RunResult result = pipeline.run();
  EXPECT_EQ(result.ended_by, EndedBy::Error);
  EXPECT_EQ(result.error, "source: decode failed");
  EXPECT_EQ(result.produced, 5u);
  expect_run_invariants(pipeline.metrics(), result.produced);
}

// Worker much slower than the source with a tiny queue: the source blocks in push, and shutdown
// at the end of the run must wake it (ER-08). Frames left behind are in flight.
TEST(PipelineTest, ShutdownWakesSourceBlockedOnFullQueue) {
  SteadyClock clock;
  PipelineConfig config = duration_config(milliseconds(300), 200.0);
  config.queue_capacity = 2;
  config.cost = milliseconds(40);
  Pipeline pipeline(config, clock, small_source(), std::make_unique<NullSink>(clock));
  const auto start = Steady::now();
  const RunResult result = pipeline.run();
  EXPECT_LT(Steady::now() - start, std::chrono::seconds(2));
  EXPECT_EQ(result.ended_by, EndedBy::Duration);
  const testing::FrameCounts counts = expect_run_invariants(pipeline.metrics(), result.produced);
  EXPECT_EQ(counts.dropped, 0u);  // blocking never drops (FR-POL-02)
  EXPECT_GT(counts.in_flight, 0u);
  EXPECT_LE(counts.in_flight, 3u);  // queue (2) + one blocked push
}

TEST(PipelineTest, RecordsDepthSamples) {
  SteadyClock clock;
  Pipeline pipeline(frames_config(10), clock, small_source(), std::make_unique<NullSink>(clock));
  const RunResult result = pipeline.run();
  const testing::FrameCounts counts = expect_run_invariants(pipeline.metrics(), result.produced);
  // One sample per push and per pop (FR-Q-07); in-flight frames were pushed but not popped.
  EXPECT_EQ(pipeline.metrics().depth_samples().size(), 2 * counts.delivered + counts.in_flight);
}

TEST(PipelineTest, UnknownPolicyIsRejected) {
  SteadyClock clock;
  PipelineConfig config = frames_config(2);
  config.policy = "leaky";
  EXPECT_THROW(Pipeline(config, clock, small_source(), std::make_unique<NullSink>(clock)),
               std::invalid_argument);
}

// 2x overload (10 ms cost at a 5 ms interval) with a small queue: dropping policies drop with the
// right reason and a drop time (FR-POL-10), never deliver a dropped frame, and keep BR-01..BR-04.
testing::FrameCounts run_overloaded(const std::string& policy) {
  SteadyClock clock;
  PipelineConfig config = duration_config(milliseconds(400), 200.0);
  config.cost = milliseconds(10);
  config.queue_capacity = 2;
  config.policy = policy;
  Pipeline pipeline(config, clock, small_source(), std::make_unique<NullSink>(clock));
  const RunResult result = pipeline.run();
  EXPECT_EQ(result.ended_by, EndedBy::Duration);
  const testing::FrameCounts counts = expect_run_invariants(pipeline.metrics(), result.produced);
  const DropReason expected = policy == "drop-oldest" ? DropReason::Oldest : DropReason::Newest;
  for (const FrameMeta& meta : pipeline.metrics().frames()) {
    if (!meta.is_dropped()) continue;
    EXPECT_EQ(meta.drop_reason, expected) << "frame " << meta.id;
    EXPECT_TRUE(meta.t_drop.has_value()) << "frame " << meta.id;
    if (meta.t_drop) {
      EXPECT_GE(*meta.t_drop, *meta.t_cap) << "frame " << meta.id;
    }
  }
  return counts;
}

TEST(PipelineTest, DropOldestDropsUnderOverload) {
  const testing::FrameCounts counts = run_overloaded("drop-oldest");
  EXPECT_GT(counts.dropped, 0u);
  EXPECT_GT(counts.delivered, 0u);
}

TEST(PipelineTest, DropNewestDropsUnderOverload) {
  const testing::FrameCounts counts = run_overloaded("drop-newest");
  EXPECT_GT(counts.dropped, 0u);
  EXPECT_GT(counts.delivered, 0u);
}

TEST(PipelineTest, BlockingNeverDropsUnderOverload) {
  EXPECT_EQ(run_overloaded("blocking").dropped, 0u);
}

TEST(PipelineTest, SecondRunIsRejected) {
  SteadyClock clock;
  Pipeline pipeline(frames_config(2), clock, small_source(), std::make_unique<NullSink>(clock));
  pipeline.run();
  EXPECT_THROW(pipeline.run(), std::logic_error);
}

TEST(PipelineTest, DestroyWithoutRunDoesNotHang) {
  SteadyClock clock;
  Pipeline pipeline(frames_config(2), clock, small_source(), std::make_unique<NullSink>(clock));
}

}  // namespace
}  // namespace frameflow
