#include "frameflow/worker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

#include "frameflow/policy/blocking_policy.hpp"
#include "frameflow/sink/null_sink.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using Steady = std::chrono::steady_clock;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(5);

FramePtr make_frame(std::uint64_t id, Clock::time_point t_cap) {
  FramePtr frame = std::make_unique<Frame>(id, cv::Mat(2, 2, CV_8UC1, cv::Scalar(0)));
  frame->meta.t_cap = t_cap;
  frame->meta.t_push = t_cap;
  return frame;
}

// Sink wrapper: stamps like NullSink, then runs a hook (count, shut the queue down, or throw).
class HookSink final : public ISink {
 public:
  HookSink(Clock& clock, std::function<void(Frame&)> after) : inner_(clock), after_(after) {}
  void consume(Frame& frame) override {
    inner_.consume(frame);
    after_(frame);
  }

 private:
  NullSink inner_;
  std::function<void(Frame&)> after_;
};

TEST(NullSinkTest, StampsOutputTime) {
  FakeClock clock(kT0);
  NullSink sink(clock);
  Frame frame(0, cv::Mat());
  sink.consume(frame);
  EXPECT_EQ(frame.meta.t_out, kT0);
}

// Single-threaded: the sink shuts the queue down after the last frame, so run() returns.
TEST(WorkerTest, StampsEveryStageAndRecordsFramesInOrder) {
  FakeClock clock(kT0);
  BlockingPolicy policy;
  BoundedQueue<FramePtr> queue(8, policy);
  MetricsStore metrics(8);
  const CostModel cost(clock, milliseconds(10), CostWait::Sleep);  // FakeClock-safe cost
  int consumed = 0;
  HookSink sink(clock, [&](Frame&) {
    if (++consumed == 3) queue.shutdown();
  });
  for (std::uint64_t id = 0; id < 3; ++id)
    ASSERT_EQ(queue.push(make_frame(id, kT0)), PushResult::Queued);

  Worker(queue, cost, sink, clock, metrics).run();

  const std::vector<FrameMeta> frames = metrics.frames();
  ASSERT_EQ(frames.size(), 3u);
  for (std::uint64_t id = 0; id < 3; ++id) {
    const FrameMeta& meta = frames[id];
    const Clock::time_point start = kT0 + milliseconds(10 * id);  // previous frames' cost
    EXPECT_EQ(meta.id, id);
    EXPECT_EQ(meta.t_pop, start);
    EXPECT_EQ(meta.t_proc_start, start);
    EXPECT_EQ(meta.t_proc_end, start + milliseconds(10));
    EXPECT_EQ(meta.t_out, start + milliseconds(10));
    EXPECT_FALSE(meta.is_dropped());
  }
}

TEST(WorkerTest, ExitsPromptlyWhenShutDownWhileWaiting) {
  SteadyClock clock;
  BlockingPolicy policy;
  BoundedQueue<FramePtr> queue(2, policy);
  MetricsStore metrics(2);
  const CostModel cost(clock, Clock::duration::zero(), CostWait::Busy);
  NullSink sink(clock);
  Worker worker(queue, cost, sink, clock, metrics);

  std::thread thread([&] { worker.run(); });
  std::this_thread::sleep_for(milliseconds(150));  // at least one 100 ms pop timeout passes
  const auto start = Steady::now();
  queue.shutdown();
  thread.join();
  EXPECT_LT(Steady::now() - start, milliseconds(50));  // woken by shutdown, not by a timeout
  EXPECT_TRUE(metrics.frames().empty());
}

TEST(WorkerTest, FrameThatThrowsIsRecordedInFlightAndExceptionPropagates) {
  FakeClock clock(kT0);
  BlockingPolicy policy;
  BoundedQueue<FramePtr> queue(2, policy);
  MetricsStore metrics(2);
  const CostModel cost(clock, Clock::duration::zero(), CostWait::Sleep);
  HookSink sink(clock, [](Frame&) { throw std::runtime_error("sink failed"); });
  ASSERT_EQ(queue.push(make_frame(0, kT0)), PushResult::Queued);

  Worker worker(queue, cost, sink, clock, metrics);
  EXPECT_THROW(worker.run(), std::runtime_error);

  const std::vector<FrameMeta> frames = metrics.frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_FALSE(frames[0].t_out.has_value());  // in-flight, not delivered
  EXPECT_TRUE(frames[0].t_pop.has_value());
}

// Producer and worker on separate threads (run under TSan): every frame delivered once, in order.
TEST(WorkerTest, DeliversEveryFrameFromAnotherThread) {
  constexpr std::uint64_t kFrames = 500;
  SteadyClock clock;
  BlockingPolicy policy;
  BoundedQueue<FramePtr> queue(4, policy);
  MetricsStore metrics(kFrames);
  const CostModel cost(clock, Clock::duration::zero(), CostWait::Busy);
  std::atomic<std::uint64_t> consumed{0};
  HookSink sink(clock, [&](Frame&) { ++consumed; });
  Worker worker(queue, cost, sink, clock, metrics);

  std::thread worker_thread([&] { worker.run(); });
  for (std::uint64_t id = 0; id < kFrames; ++id)
    ASSERT_EQ(queue.push(make_frame(id, clock.now())), PushResult::Queued);
  while (consumed < kFrames) std::this_thread::yield();
  queue.shutdown();
  worker_thread.join();

  const std::vector<FrameMeta> frames = metrics.frames();
  ASSERT_EQ(frames.size(), kFrames);
  for (std::uint64_t id = 1; id < kFrames; ++id) {
    EXPECT_LE(*frames[id - 1].t_out, *frames[id].t_out);  // FIFO all the way to the sink
  }
}

}  // namespace
}  // namespace frameflow
