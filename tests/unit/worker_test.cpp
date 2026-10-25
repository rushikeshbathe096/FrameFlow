#include "frameflow/worker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

#include "frameflow/policy/adaptive_policy.hpp"
#include "frameflow/policy/blocking_policy.hpp"
#include "frameflow/processor/image_processors.hpp"
#include "frameflow/sink/null_sink.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using Steady = std::chrono::steady_clock;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(5);
const NoneProcessor kNone;

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

  Worker(queue, kNone, cost, sink, clock, metrics, policy).run();

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

// P4.4 (FR-POL-08 pop part): while the adaptive policy is SHEDDING, a frame older than T at pop
// is dropped as `stale` instead of being processed; a fresh frame is processed and its latency
// is fed back to the policy.
struct AdaptiveWorkerFixture {
  FakeClock clock{kT0};
  AdaptivePolicy policy{clock, AdaptiveParams{milliseconds(100), 0.7, 5}};
  BoundedQueue<FramePtr> queue{8, policy};
  MetricsStore metrics{8};
  const CostModel cost{clock, milliseconds(10), CostWait::Sleep};

  // Queues a 200 ms old frame (id 0) and a fresh one (id 1) while still NORMAL.
  void queue_stale_and_fresh() {
    clock.advance(milliseconds(200));
    ASSERT_EQ(queue.push(make_frame(0, kT0)), PushResult::Queued);
    ASSERT_EQ(queue.push(make_frame(1, clock.now())), PushResult::Queued);
  }
  // Stops after `delivered` deliveries, or after 2 s (watchdog) so a regression fails instead of
  // hanging the suite.
  void run_until(int delivered) {
    std::atomic<bool> done{false};
    int consumed = 0;
    HookSink sink(clock, [&](Frame&) {
      if (++consumed == delivered) queue.shutdown();
    });
    std::thread watchdog([&] {
      for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(milliseconds(10));
      queue.shutdown();
    });
    Worker(queue, kNone, cost, sink, clock, metrics, policy).run();
    done = true;
    watchdog.join();
  }
};

TEST(WorkerTest, SheddingDropsStaleFrameOnPop) {
  AdaptiveWorkerFixture f;
  f.queue_stale_and_fresh();
  f.policy.on_delivered(milliseconds(500), Clock::duration::zero(),
                        f.clock.now());  // L = 500 > T: SHEDDING
  ASSERT_EQ(f.policy.state(), AdaptiveState::Shedding);
  f.run_until(1);
  const std::vector<FrameMeta> frames = f.metrics.frames();
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].drop_reason, DropReason::Stale);
  EXPECT_TRUE(frames[0].t_drop.has_value());
  EXPECT_EQ(frames[0].t_drop, frames[0].t_pop);
  EXPECT_FALSE(frames[0].t_out.has_value());         // never delivered (BR-02)
  EXPECT_FALSE(frames[0].t_proc_start.has_value());  // never processed
  EXPECT_FALSE(frames[1].is_dropped());
  EXPECT_TRUE(frames[1].t_out.has_value());
}

TEST(WorkerTest, NormalProcessesStaleFrame) {
  AdaptiveWorkerFixture f;
  f.queue_stale_and_fresh();
  f.run_until(2);  // BR-07: NORMAL never drops, however old the frame
  for (const FrameMeta& meta : f.metrics.frames()) EXPECT_FALSE(meta.is_dropped()) << meta.id;
  EXPECT_EQ(f.policy.latency_estimate(), milliseconds(210));  // 200 ms queued + 10 ms cost
}

TEST(WorkerTest, DeliveredLatencyIsFedBackToThePolicy) {
  AdaptiveWorkerFixture f;
  ASSERT_EQ(f.queue.push(make_frame(0, f.clock.now() - milliseconds(150))), PushResult::Queued);
  f.run_until(1);
  EXPECT_EQ(f.policy.latency_estimate(), milliseconds(160));
  EXPECT_EQ(f.policy.state(), AdaptiveState::Shedding);  // 160 ms > T
}

TEST(WorkerTest, ExitsPromptlyWhenShutDownWhileWaiting) {
  SteadyClock clock;
  BlockingPolicy policy;
  BoundedQueue<FramePtr> queue(2, policy);
  MetricsStore metrics(2);
  const CostModel cost(clock, Clock::duration::zero(), CostWait::Busy);
  NullSink sink(clock);
  Worker worker(queue, kNone, cost, sink, clock, metrics, policy);

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

  Worker worker(queue, kNone, cost, sink, clock, metrics, policy);
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
  Worker worker(queue, kNone, cost, sink, clock, metrics, policy);

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
