#include "frameflow/policy/adaptive_policy.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "frameflow/bounded_queue.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(100);

// T = 100 ms, T_low = 70 ms, W = 5.
AdaptiveParams params() { return AdaptiveParams{milliseconds(100), 0.7, 5}; }

FrameMeta captured_at(Clock::time_point t_cap, std::uint64_t id = 0) {
  FrameMeta meta;
  meta.id = id;
  meta.t_cap = t_cap;
  return meta;
}

void deliver(AdaptivePolicy& policy, FakeClock& clock, milliseconds latency, int times = 1) {
  for (int i = 0; i < times; ++i) {
    clock.advance(milliseconds(1));
    policy.on_delivered(latency, Clock::duration::zero(), clock.now());
  }
}

// ---------------------------------------------------------------- P4.2 state machine

TEST(AdaptivePolicyTest, StartsNormalWithoutEstimate) {
  FakeClock clock(kT0);
  const AdaptivePolicy policy(clock, params());
  EXPECT_EQ(policy.state(), AdaptiveState::Normal);
  EXPECT_FALSE(policy.latency_estimate().has_value());  // EC-14
  EXPECT_TRUE(policy.state_changes().empty());
  EXPECT_EQ(policy.name(), "adaptive");
}

// FR-POL-07: NORMAL behaves like blocking.
TEST(AdaptivePolicyTest, NormalBehavesLikeBlocking) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  EXPECT_EQ(policy.on_push(QueueView{3, 8}, captured_at(kT0)), PushDecision::Insert);
  EXPECT_EQ(policy.on_push(QueueView{8, 8}, captured_at(kT0)), PushDecision::Wait);
}

// FR-POL-06: enter SHEDDING when L > T.
TEST(AdaptivePolicyTest, EntersSheddingAboveTarget) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  deliver(policy, clock, milliseconds(100));  // L = 100: not above T
  EXPECT_EQ(policy.state(), AdaptiveState::Normal);
  deliver(policy, clock, milliseconds(101));  // p95 of {100, 101} = 101 > T
  EXPECT_EQ(policy.state(), AdaptiveState::Shedding);
  EXPECT_EQ(policy.latency_estimate(), milliseconds(101));
}

// FR-POL-06: leave SHEDDING only when L < T_low (70 ms), once the window has forgotten the spike.
TEST(AdaptivePolicyTest, ExitsBelowLowThresholdAfterWindowSlides) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  deliver(policy, clock, milliseconds(300));
  ASSERT_EQ(policy.state(), AdaptiveState::Shedding);
  deliver(policy, clock, milliseconds(50), 4);  // window {300, 50, 50, 50, 50}: p95 = 300
  EXPECT_EQ(policy.state(), AdaptiveState::Shedding);
  deliver(policy, clock, milliseconds(50));  // window all 50: L = 50 < 70
  EXPECT_EQ(policy.state(), AdaptiveState::Normal);
}

TEST(AdaptivePolicyTest, ExactlyLowThresholdDoesNotExit) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  deliver(policy, clock, milliseconds(150));
  deliver(policy, clock, milliseconds(70), 5);  // L = 70: not below T_low
  EXPECT_EQ(policy.state(), AdaptiveState::Shedding);
  deliver(policy, clock, milliseconds(69), 5);
  EXPECT_EQ(policy.state(), AdaptiveState::Normal);
}

// EC-13: latency hovering inside [T_low, T] never changes the state, whichever state it is in.
TEST(AdaptivePolicyTest, NoFlappingInsideTheBand) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  const int band[] = {70, 85, 100, 75, 99, 71, 100, 90};  // ms, all inside [T_low, T]
  for (int round = 0; round < 50; ++round) {
    for (int ms : band) deliver(policy, clock, milliseconds(ms));
  }
  EXPECT_EQ(policy.state(), AdaptiveState::Normal);
  deliver(policy, clock, milliseconds(500));  // one excursion above T
  ASSERT_EQ(policy.state(), AdaptiveState::Shedding);
  for (int round = 0; round < 50; ++round) {
    for (int ms : band) deliver(policy, clock, milliseconds(ms));
  }
  EXPECT_EQ(policy.state(), AdaptiveState::Shedding);
  EXPECT_EQ(policy.state_changes().size(), 1u);
}

// A single threshold would flip on every sample here; hysteresis gives two changes in total.
TEST(AdaptivePolicyTest, AlternatingAroundTargetChangesOnlyOnce) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  for (int i = 0; i < 200; ++i) deliver(policy, clock, milliseconds(i % 2 == 0 ? 95 : 105));
  EXPECT_EQ(policy.state_changes().size(), 1u);
}

// FR-POL-09: every change has time, old state, new state and L.
TEST(AdaptivePolicyTest, LogsStateChangesWithTimeStatesAndEstimate) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  deliver(policy, clock, milliseconds(200));
  const Clock::time_point entered = clock.now();
  deliver(policy, clock, milliseconds(10), 5);
  const Clock::time_point left = clock.now();
  const std::vector<StateChange> changes = policy.state_changes();
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].t, entered);
  EXPECT_EQ(changes[0].from, AdaptiveState::Normal);
  EXPECT_EQ(changes[0].to, AdaptiveState::Shedding);
  EXPECT_EQ(changes[0].latency_estimate, milliseconds(200));
  EXPECT_EQ(changes[1].t, left);
  EXPECT_EQ(changes[1].to, AdaptiveState::Normal);
  EXPECT_EQ(changes[1].latency_estimate, milliseconds(10));
  EXPECT_EQ(state_name(AdaptiveState::Shedding), "SHEDDING");
}

TEST(AdaptivePolicyTest, RejectsInvalidParameters) {
  FakeClock clock(kT0);
  EXPECT_THROW(AdaptivePolicy(clock, AdaptiveParams{milliseconds(0), 0.7, 5}),
               std::invalid_argument);
  EXPECT_THROW(AdaptivePolicy(clock, AdaptiveParams{milliseconds(100), 1.0, 5}),
               std::invalid_argument);
  EXPECT_THROW(AdaptivePolicy(clock, AdaptiveParams{milliseconds(100), 0.0, 5}),
               std::invalid_argument);
  EXPECT_THROW(AdaptivePolicy(clock, AdaptiveParams{milliseconds(100), 0.7, 0}),
               std::invalid_argument);
}

// ---------------------------------------------------------------- shedding decisions

TEST(AdaptivePolicyTest, SheddingPushAsksForStaleRemovalEvenWhenNotFull) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  deliver(policy, clock, milliseconds(500));
  EXPECT_EQ(policy.on_push(QueueView{2, 8}, captured_at(clock.now())), PushDecision::DropStale);
  EXPECT_EQ(policy.on_push(QueueView{8, 8}, captured_at(clock.now())), PushDecision::DropStale);
}

TEST(AdaptivePolicyTest, StaleMeansOlderThanTarget) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  const Clock::time_point now = clock.now();
  EXPECT_FALSE(policy.is_stale(captured_at(now - milliseconds(100))));  // exactly T: not stale
  EXPECT_TRUE(policy.is_stale(captured_at(now - milliseconds(101))));
}

// BR-07: NORMAL never drops, even a very old frame.
TEST(AdaptivePolicyTest, NormalNeverDropsOnPop) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  EXPECT_FALSE(policy.should_drop_on_pop(captured_at(kT0 - std::chrono::seconds(10)), kT0));
}

// FR-POL-08 (pop part).
TEST(AdaptivePolicyTest, SheddingDropsOnlyStaleFramesOnPop) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  deliver(policy, clock, milliseconds(500));
  const Clock::time_point now = clock.now();
  EXPECT_TRUE(policy.should_drop_on_pop(captured_at(now - milliseconds(150)), now));
  EXPECT_FALSE(policy.should_drop_on_pop(captured_at(now - milliseconds(100)), now));
  EXPECT_FALSE(policy.should_drop_on_pop(captured_at(now - milliseconds(5)), now));
}

// ---------------------------------------------------------------- D47 predicted delivery age

TEST(AdaptivePolicyTest, CostEstimateIsEwmaStartingAtZero) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  EXPECT_EQ(policy.estimated_cost(), Clock::duration::zero());
  policy.on_delivered(milliseconds(60), milliseconds(50), clock.now());
  EXPECT_EQ(policy.estimated_cost(), milliseconds(10));  // 0.2 x 50 + 0.8 x 0
  policy.on_delivered(milliseconds(60), milliseconds(50), clock.now());
  EXPECT_EQ(policy.estimated_cost(), milliseconds(18));  // 0.2 x 50 + 0.8 x 10
  policy.on_delivered(milliseconds(60), Clock::duration::zero(), clock.now());
  EXPECT_EQ(policy.estimated_cost(), std::chrono::microseconds(14400));  // 0.8 x 18
}

// est_cost = 10 ms after one delivery with 50 ms of processing; that delivery (L = 500 ms) also
// switches to SHEDDING.
AdaptivePolicy* shedding_with_cost(FakeClock& clock, std::unique_ptr<AdaptivePolicy>& out) {
  out = std::make_unique<AdaptivePolicy>(clock, params());
  out->on_delivered(milliseconds(500), milliseconds(50), clock.now());
  EXPECT_EQ(out->state(), AdaptiveState::Shedding);
  EXPECT_EQ(out->estimated_cost(), milliseconds(10));
  return out.get();
}

// A frame younger than T but older than T - est_cost would arrive late: drop it before
// processing instead of processing it and delivering it late.
TEST(AdaptivePolicyTest, PopDropsFrameThatWouldBeLateOnDelivery) {
  FakeClock clock(kT0);
  std::unique_ptr<AdaptivePolicy> owner;
  AdaptivePolicy& policy = *shedding_with_cost(clock, owner);
  const Clock::time_point now = clock.now();
  EXPECT_TRUE(policy.should_drop_on_pop(captured_at(now - milliseconds(95)), now));   // 105 > T
  EXPECT_FALSE(policy.should_drop_on_pop(captured_at(now - milliseconds(90)), now));  // = T
  EXPECT_FALSE(policy.should_drop_on_pop(captured_at(now - milliseconds(50)), now));
}

TEST(AdaptivePolicyTest, PushUsesTheSamePredictedAgeRule) {
  FakeClock clock(kT0);
  std::unique_ptr<AdaptivePolicy> owner;
  AdaptivePolicy& policy = *shedding_with_cost(clock, owner);
  const Clock::time_point now = clock.now();
  EXPECT_TRUE(policy.is_stale(captured_at(now - milliseconds(95))));
  EXPECT_FALSE(policy.is_stale(captured_at(now - milliseconds(90))));
}

TEST(AdaptivePolicyTest, NormalStillNeverDropsWithACostEstimate) {
  FakeClock clock(kT0);
  AdaptivePolicy policy(clock, params());
  policy.on_delivered(milliseconds(20), milliseconds(80), clock.now());  // est 16 ms, NORMAL
  ASSERT_EQ(policy.state(), AdaptiveState::Normal);
  EXPECT_FALSE(policy.should_drop_on_pop(captured_at(kT0 - milliseconds(99)), kT0));  // BR-07
}

// ---------------------------------------------------------------- P4.3 queue shedding

struct TestItem {
  FrameMeta meta;
};
using Item = std::unique_ptr<TestItem>;

Item item(std::uint64_t id, Clock::time_point t_cap) {
  Item out = std::make_unique<TestItem>();
  out->meta = captured_at(t_cap, id);
  return out;
}

struct Drop {
  std::uint64_t id;
  DropReason reason;
};

struct SheddingQueue {
  FakeClock clock{kT0};
  AdaptivePolicy policy{clock, params()};
  std::vector<Drop> drops;
  BoundedQueue<Item> queue{4, policy, {}, [this](Item dropped) {
                             drops.push_back({dropped->meta.id, dropped->meta.drop_reason});
                           }};

  void enter_shedding() { deliver(policy, clock, milliseconds(500)); }
  std::vector<std::uint64_t> ids() {
    std::vector<std::uint64_t> out;
    while (std::optional<Item> popped = queue.pop(milliseconds(0))) {
      out.push_back((*popped)->meta.id);
    }
    return out;
  }
};

TEST(AdaptiveQueueTest, SheddingRemovesStaleFramesInOrderAndKeepsFresh) {
  SheddingQueue q;
  const Clock::time_point start = q.clock.now();
  ASSERT_EQ(q.queue.push(item(0, start)), PushResult::Queued);  // NORMAL: plain inserts
  ASSERT_EQ(q.queue.push(item(1, start + milliseconds(30))), PushResult::Queued);
  ASSERT_EQ(q.queue.push(item(2, start + milliseconds(90))), PushResult::Queued);
  q.clock.advance(milliseconds(150));  // ages now 150, 120, 60 ms
  q.enter_shedding();
  ASSERT_EQ(q.queue.push(item(3, q.clock.now())), PushResult::Queued);
  ASSERT_EQ(q.drops.size(), 2u);
  EXPECT_EQ(q.drops[0].id, 0u);
  EXPECT_EQ(q.drops[0].reason, DropReason::Stale);
  EXPECT_EQ(q.drops[1].id, 1u);
  EXPECT_EQ(q.drops[1].reason, DropReason::Stale);
  EXPECT_EQ(q.ids(), (std::vector<std::uint64_t>{2, 3}));
}

// D47 on push: with est_cost = 10 ms, a 95 ms old frame is shed although it is younger than T.
TEST(AdaptiveQueueTest, SheddingUsesPredictedDeliveryAge) {
  SheddingQueue q;
  const Clock::time_point start = q.clock.now();
  ASSERT_EQ(q.queue.push(item(0, start)), PushResult::Queued);
  ASSERT_EQ(q.queue.push(item(1, start + milliseconds(10))), PushResult::Queued);
  q.clock.advance(milliseconds(95));                                          // ages 95 and 85 ms
  q.policy.on_delivered(milliseconds(500), milliseconds(50), q.clock.now());  // SHEDDING, 10 ms
  ASSERT_EQ(q.queue.push(item(2, q.clock.now())), PushResult::Queued);
  ASSERT_EQ(q.drops.size(), 1u);
  EXPECT_EQ(q.drops[0].id, 0u);  // 95 + 10 > 100; frame 1: 85 + 10 <= 100 stays
  EXPECT_EQ(q.ids(), (std::vector<std::uint64_t>{1, 2}));
}

TEST(AdaptiveQueueTest, StillFullAfterStaleRemovalDropsOldest) {
  SheddingQueue q;
  q.enter_shedding();
  for (std::uint64_t id = 0; id < 4; ++id) {  // fresh frames fill the queue (capacity 4)
    ASSERT_EQ(q.queue.push(item(id, q.clock.now())), PushResult::Queued);
  }
  ASSERT_EQ(q.queue.push(item(4, q.clock.now())), PushResult::Queued);
  ASSERT_EQ(q.drops.size(), 1u);
  EXPECT_EQ(q.drops[0].id, 0u);
  EXPECT_EQ(q.drops[0].reason, DropReason::Oldest);
  EXPECT_EQ(q.ids(), (std::vector<std::uint64_t>{1, 2, 3, 4}));
}

TEST(AdaptiveQueueTest, CapacityAlwaysRespected) {
  SheddingQueue q;
  q.enter_shedding();
  for (std::uint64_t id = 0; id < 50; ++id) {
    q.clock.advance(milliseconds(id % 3 == 0 ? 120 : 10));
    ASSERT_EQ(q.queue.push(item(id, q.clock.now())), PushResult::Queued);
    ASSERT_LE(q.queue.size(), 4u);
  }
  EXPECT_EQ(q.drops.size() + q.queue.size(), 50u);  // every frame is queued or dropped once
}

TEST(AdaptiveQueueTest, NormalNeverDropsAndBlocksWhenFull) {
  SheddingQueue q;
  for (std::uint64_t id = 0; id < 4; ++id) {
    ASSERT_EQ(q.queue.push(item(id, q.clock.now() - std::chrono::seconds(5))), PushResult::Queued);
  }
  std::thread consumer([&] {
    std::this_thread::sleep_for(milliseconds(20));
    q.queue.pop(milliseconds(1000));
  });
  EXPECT_EQ(q.queue.push(item(4, q.clock.now())), PushResult::Queued);  // waited for space
  consumer.join();
  EXPECT_TRUE(q.drops.empty());  // BR-07, even though every queued frame is 5 s old
}

// ---------------------------------------------------------------- P4.4 thread safety

// The source thread calls on_push/is_stale (under the queue lock) while the worker thread calls
// should_drop_on_pop/on_delivered. Run under TSan; the state must stay consistent.
TEST(AdaptivePolicyTest, ConcurrentSourceAndWorkerCallsAreRaceFree) {
  SteadyClock clock;
  AdaptivePolicy policy(clock, params());
  std::atomic<bool> stop{false};
  std::thread source([&] {
    while (!stop) {
      const FrameMeta meta = captured_at(clock.now() - milliseconds(120));
      const PushDecision decision = policy.on_push(QueueView{4, 4}, meta);
      if (decision == PushDecision::DropStale) {
        EXPECT_TRUE(policy.is_stale(meta));
      }
    }
  });
  for (int i = 0; i < 20000; ++i) {
    policy.on_delivered(milliseconds(i % 400), milliseconds(i % 50), clock.now());
    policy.should_drop_on_pop(captured_at(clock.now() - milliseconds(150)), clock.now());
  }
  stop = true;
  source.join();
  const std::vector<StateChange> changes = policy.state_changes();
  for (std::size_t i = 1; i < changes.size(); ++i) {
    EXPECT_EQ(changes[i].from, changes[i - 1].to);  // a consistent sequence of transitions
    EXPECT_LE(changes[i - 1].t, changes[i].t);
  }
}

}  // namespace
}  // namespace frameflow
