#include "frameflow/source/source_runner.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "frameflow/policy/blocking_policy.hpp"
#include "frameflow/source/synthetic_source.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using Steady = std::chrono::steady_clock;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(10);

// Test source: tiny frames, plus a hook that runs before each frame is created.
class HookSource final : public ISource {
 public:
  explicit HookSource(std::function<bool(std::uint64_t)> before_next = {})
      : before_next_(std::move(before_next)) {}
  FramePtr next(std::uint64_t id) override {
    if (before_next_ && !before_next_(id)) return nullptr;  // hook returns false: input ended
    return std::make_unique<Frame>(id, cv::Mat(2, 2, CV_8UC1, cv::Scalar(0)));
  }

 private:
  std::function<bool(std::uint64_t)> before_next_;
};

std::vector<FramePtr> drain(BoundedQueue<FramePtr>& queue) {
  std::vector<FramePtr> out;
  while (std::optional<FramePtr> frame = queue.pop(milliseconds(0)))
    out.push_back(std::move(*frame));
  return out;
}

struct Fixture {
  FakeClock clock{kT0};
  BlockingPolicy policy;
  BoundedQueue<FramePtr> queue{64, policy};
  MetricsStore metrics{64};
  std::atomic<bool> stop{false};
};

TEST(SourceRunnerTest, ReleasesOnScheduleWithSequentialIds) {
  Fixture f;
  HookSource source;
  SourceRunner runner(source, f.clock, f.queue, f.metrics, 25.0, 5);  // 40 ms interval

  EXPECT_EQ(runner.run(kT0, f.stop), 5u);

  const std::vector<FramePtr> frames = drain(f.queue);
  ASSERT_EQ(frames.size(), 5u);
  for (std::uint64_t k = 0; k < 5; ++k) {
    const FrameMeta& meta = frames[k]->meta;
    EXPECT_EQ(meta.id, k);  // FR-SRC-05: 0, 1, 2... no gaps
    EXPECT_EQ(meta.t_cap, kT0 + milliseconds(40 * k));
    EXPECT_EQ(meta.source_lateness, Clock::duration::zero());
    EXPECT_EQ(meta.t_push, meta.t_cap);
  }
  EXPECT_TRUE(f.metrics.frames().empty());  // delivered/in-flight recording is not the source's job
}

TEST(SourceRunnerTest, ReleaseTimeUsesExactFractionalInterval) {
  Fixture f;
  HookSource source;
  const SourceRunner runner(source, f.clock, f.queue, f.metrics, 30.0, 1);
  EXPECT_EQ(runner.release_time(kT0, 3), kT0 + milliseconds(100));  // 3 / 30 s
  EXPECT_EQ(runner.release_time(kT0, 30), kT0 + std::chrono::seconds(1));
  EXPECT_EQ(runner.release_time(kT0, 1), kT0 + std::chrono::nanoseconds(33333333));
}

// EC-12: a slow source (50 ms per frame at a 40 ms interval) falls behind its own schedule.
TEST(SourceRunnerTest, LateReleaseKeepsScheduledCaptureTimeAndRecordsLateness) {
  Fixture f;
  HookSource source([&](std::uint64_t) {
    f.clock.advance(milliseconds(50));
    return true;
  });
  SourceRunner runner(source, f.clock, f.queue, f.metrics, 25.0, 3);

  EXPECT_EQ(runner.run(kT0, f.stop), 3u);

  const std::vector<FramePtr> frames = drain(f.queue);
  ASSERT_EQ(frames.size(), 3u);
  for (std::uint64_t k = 0; k < 3; ++k) {
    const FrameMeta& meta = frames[k]->meta;
    EXPECT_EQ(meta.t_cap, kT0 + milliseconds(40 * k));  // FR-SRC-04: scheduled, not actual
    // Frame k is released at 50 (k+1) ms after t0, scheduled at 40 k ms.
    EXPECT_EQ(meta.source_lateness, milliseconds(50 * (k + 1) - 40 * k));
    EXPECT_EQ(meta.t_push, kT0 + milliseconds(50 * (k + 1)));
  }
}

// D38: the tracker splits lateness into backpressure and the source's own part.
TEST(BackpressureTrackerTest, NoPushWaitMeansAllLatenessIsOwn) {
  BackpressureTracker tracker;
  EXPECT_EQ(tracker.on_release(milliseconds(30)), Clock::duration::zero());
}

TEST(BackpressureTrackerTest, LatenessFromBlockedPushIsBackpressure) {
  BackpressureTracker tracker;
  tracker.on_push_wait(milliseconds(100));  // blocked on a full queue for 100 ms
  // The next frame was released 60 ms late, all of it because of the wait.
  EXPECT_EQ(tracker.on_release(milliseconds(60)), milliseconds(60));
}

TEST(BackpressureTrackerTest, CaughtUpWaitIsForgotten) {
  BackpressureTracker tracker;
  tracker.on_push_wait(milliseconds(500));
  EXPECT_EQ(tracker.on_release(milliseconds(500)), milliseconds(500));
  EXPECT_EQ(tracker.on_release(milliseconds(0)), Clock::duration::zero());  // back on schedule
  // A later slow phase of the source itself is not hidden by the old wait.
  EXPECT_EQ(tracker.on_release(milliseconds(20)), Clock::duration::zero());
}

TEST(BackpressureTrackerTest, OwnSlownessOnTopOfWaitIsKept) {
  BackpressureTracker tracker;
  tracker.on_push_wait(milliseconds(50));
  EXPECT_EQ(tracker.on_release(milliseconds(80)), milliseconds(50));  // 30 ms is the source's own
}

TEST(SourceRunnerTest, SlowSourceWithoutBlockingHasNoBackpressure) {
  Fixture f;
  HookSource source([&](std::uint64_t) {
    f.clock.advance(milliseconds(50));  // slower than the 40 ms interval; the queue never fills
    return true;
  });
  SourceRunner runner(source, f.clock, f.queue, f.metrics, 25.0, 3);
  runner.run(kT0, f.stop);
  for (const FramePtr& frame : drain(f.queue)) {
    EXPECT_EQ(frame->meta.backpressure, Clock::duration::zero()) << frame->meta.id;
    EXPECT_EQ(frame->meta.own_lateness(), frame->meta.source_lateness);
  }
}

TEST(SourceRunnerTest, StopBeforeStartProducesNothing) {
  Fixture f;
  f.stop = true;
  HookSource source;
  SourceRunner runner(source, f.clock, f.queue, f.metrics, 30.0, 10);
  EXPECT_EQ(runner.run(kT0, f.stop), 0u);
  EXPECT_EQ(f.queue.size(), 0u);
}

TEST(SourceRunnerTest, StopDuringRunEndsLoop) {
  Fixture f;
  HookSource source([&](std::uint64_t id) {
    if (id == 3) f.stop = true;  // stop is requested while frame 3 waits for its release
    return true;
  });
  SourceRunner runner(source, f.clock, f.queue, f.metrics, 30.0, 10);
  EXPECT_EQ(runner.run(kT0, f.stop), 3u);  // frame 3 was never released
  EXPECT_EQ(f.queue.size(), 3u);
}

TEST(SourceRunnerTest, EndOfInputEndsLoop) {
  Fixture f;
  HookSource source([](std::uint64_t id) { return id < 2; });
  SourceRunner runner(source, f.clock, f.queue, f.metrics, 30.0, 10);
  EXPECT_EQ(runner.run(kT0, f.stop), 2u);
  EXPECT_EQ(runner.produced(), 2u);
}

TEST(SourceRunnerTest, ProducedCountSurvivesSourceException) {
  Fixture f;
  HookSource source([](std::uint64_t id) {
    if (id == 3) throw std::runtime_error("decode failed");
    return true;
  });
  SourceRunner runner(source, f.clock, f.queue, f.metrics, 30.0, 10);
  EXPECT_THROW(runner.run(kT0, f.stop), std::runtime_error);
  EXPECT_EQ(runner.produced(), 3u);  // frames 0-2 were released before the failure
}

TEST(SourceRunnerTest, PushRejectedByShutdownIsRecordedInFlight) {
  Fixture f;
  f.queue.shutdown();
  HookSource source;
  SourceRunner runner(source, f.clock, f.queue, f.metrics, 30.0, 10);

  EXPECT_EQ(runner.run(kT0, f.stop), 1u);  // released, then the push failed

  const std::vector<FrameMeta> recorded = f.metrics.frames();
  ASSERT_EQ(recorded.size(), 1u);
  EXPECT_EQ(recorded[0].id, 0u);
  EXPECT_EQ(recorded[0].t_cap, kT0);
  EXPECT_FALSE(recorded[0].t_push.has_value());  // never entered the queue
  EXPECT_FALSE(recorded[0].is_dropped());
}

TEST(SourceRunnerTest, RejectsNonPositiveFps) {
  Fixture f;
  HookSource source;
  EXPECT_THROW(SourceRunner(source, f.clock, f.queue, f.metrics, 0.0, 1), std::invalid_argument);
}

// Real clock: at 1 FPS the next release is 1 s away, but stop must end the wait in steps of at
// most 50 ms (bound kept loose for sanitizer builds).
TEST(SourceRunnerTest, StopInterruptsLongWaitQuickly) {
  SteadyClock clock;
  BlockingPolicy policy;
  BoundedQueue<FramePtr> queue(4, policy);
  MetricsStore metrics(4);
  std::atomic<bool> stop{false};
  SyntheticSource source(cv::Size(16, 16));
  SourceRunner runner(source, clock, queue, metrics, 1.0, 10);

  std::uint64_t produced = 0;
  std::thread thread([&] { produced = runner.run(clock.now(), stop); });
  ASSERT_TRUE(queue.pop(milliseconds(2000)).has_value());  // frame 0 released at once
  const auto stop_time = Steady::now();
  stop = true;
  thread.join();
  EXPECT_LT(Steady::now() - stop_time, milliseconds(300));
  EXPECT_EQ(produced, 1u);
}

}  // namespace
}  // namespace frameflow
