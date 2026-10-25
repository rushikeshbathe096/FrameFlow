#include "frameflow/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "frameflow/policy/blocking_policy.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using Steady = std::chrono::steady_clock;

// Smallest item the queue accepts (D18): move-only, pointee has `meta`. The id is the payload.
struct TestItem {
  FrameMeta meta;
};
using Item = std::unique_ptr<TestItem>;
using Queue = BoundedQueue<Item>;

Item item(std::uint64_t id) {
  Item out = std::make_unique<TestItem>();
  out->meta.id = id;
  return out;
}

std::uint64_t id_of(const std::optional<Item>& popped) { return (*popped)->meta.id; }

constexpr milliseconds kNoWait{0};
constexpr milliseconds kLongWait{10000};  // only reached if a wake-up is missing

// Gives another thread time to reach its blocking call. Used only to show that a call is still
// blocked; a correct queue passes no matter how long this sleep is.
void let_other_thread_block() { std::this_thread::sleep_for(milliseconds(50)); }

class BoundedQueueTest : public ::testing::Test {
 protected:
  BlockingPolicy blocking_;
};

// --- Single-thread semantics (P1.3) ---

TEST_F(BoundedQueueTest, ZeroCapacityIsRejected) {
  EXPECT_THROW(Queue(0, blocking_), std::invalid_argument);
}

TEST_F(BoundedQueueTest, StartsEmptyWithGivenCapacity) {
  Queue queue(3, blocking_);
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 3u);
  EXPECT_FALSE(queue.is_shutdown());
  EXPECT_FALSE(queue.pop(kNoWait).has_value());
}

TEST_F(BoundedQueueTest, DeliversInFifoOrder) {
  Queue queue(5, blocking_);
  for (std::uint64_t i = 0; i < 5; ++i) ASSERT_EQ(queue.push(item(i)), PushResult::Queued);
  for (std::uint64_t i = 0; i < 5; ++i) {
    std::optional<Item> popped = queue.pop(kNoWait);
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(id_of(popped), i);
  }
}

TEST_F(BoundedQueueTest, CapacityOneAlternatesPushAndPop) {
  Queue queue(1, blocking_);
  for (std::uint64_t i = 0; i < 10; ++i) {
    ASSERT_EQ(queue.push(item(i)), PushResult::Queued);
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(id_of(queue.pop(kNoWait)), i);
  }
}

TEST_F(BoundedQueueTest, MovesMoveOnlyItems) {
  Queue queue(2, blocking_);
  Item pushed = item(42);
  TestItem* raw = pushed.get();
  ASSERT_EQ(queue.push(std::move(pushed)), PushResult::Queued);
  EXPECT_EQ(pushed, nullptr);  // NOLINT(bugprone-use-after-move): ownership moved into the queue

  std::optional<Item> out = queue.pop(kNoWait);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->get(), raw);  // same object, not a copy
}

// --- Blocking push (P1.4, FR-POL-02) ---

TEST_F(BoundedQueueTest, PushBlocksWhenFullAndResumesAfterPop) {
  Queue queue(2, blocking_);
  ASSERT_EQ(queue.push(item(1)), PushResult::Queued);
  ASSERT_EQ(queue.push(item(2)), PushResult::Queued);

  std::atomic<bool> third_pushed{false};
  std::thread producer([&] {
    EXPECT_EQ(queue.push(item(3)), PushResult::Queued);
    third_pushed = true;
  });
  let_other_thread_block();
  EXPECT_FALSE(third_pushed);   // blocked: the queue is full
  EXPECT_EQ(queue.size(), 2u);  // capacity never exceeded (FR-Q-01)
  EXPECT_EQ(id_of(queue.pop(kNoWait)), 1u);
  producer.join();  // push completes once space is free

  EXPECT_TRUE(third_pushed);
  EXPECT_EQ(id_of(queue.pop(kNoWait)), 2u);  // the blocked item went to the back
  EXPECT_EQ(id_of(queue.pop(kNoWait)), 3u);
}

// --- Timed pop (FR-Q-05) ---

TEST_F(BoundedQueueTest, PopTimesOutNoEarlierThanTimeout) {
  Queue queue(1, blocking_);
  const auto start = Steady::now();
  EXPECT_FALSE(queue.pop(milliseconds(30)).has_value());
  EXPECT_GE(Steady::now() - start, milliseconds(30));  // lower bound only (D4)
}

TEST_F(BoundedQueueTest, PopReturnsWhenItemArrives) {
  Queue queue(1, blocking_);
  std::optional<Item> received;
  std::thread consumer([&] { received = queue.pop(kLongWait); });
  let_other_thread_block();
  ASSERT_EQ(queue.push(item(7)), PushResult::Queued);
  consumer.join();  // would take kLongWait if the push did not wake the consumer
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(id_of(received), 7u);
}

// --- Shutdown (FR-Q-06, EC-09 and EC-10 at queue level) ---

TEST_F(BoundedQueueTest, ShutdownWakesBlockedPopWithin50ms) {
  Queue queue(1, blocking_);
  std::optional<Item> received = item(99);
  std::thread consumer([&] { received = queue.pop(kLongWait); });
  let_other_thread_block();

  const auto start = Steady::now();
  queue.shutdown();
  consumer.join();
  EXPECT_LT(Steady::now() - start, milliseconds(50));
  EXPECT_FALSE(received.has_value());
}

TEST_F(BoundedQueueTest, ShutdownWakesBlockedPushWithin50ms) {
  Queue queue(1, blocking_);
  ASSERT_EQ(queue.push(item(1)), PushResult::Queued);
  Item blocked = item(2);
  bool pushed = true;
  std::thread producer([&] { pushed = queue.push(std::move(blocked)) == PushResult::Queued; });
  let_other_thread_block();

  const auto start = Steady::now();
  queue.shutdown();
  producer.join();
  EXPECT_LT(Steady::now() - start, milliseconds(50));
  EXPECT_FALSE(pushed);
  ASSERT_NE(blocked, nullptr);  // NOLINT(bugprone-use-after-move): failed push must not consume it
  EXPECT_EQ(blocked->meta.id, 2u);
}

TEST_F(BoundedQueueTest, AfterShutdownPushFailsAndPopReturnsNothing) {
  Queue queue(2, blocking_);
  ASSERT_EQ(queue.push(item(1)), PushResult::Queued);
  queue.shutdown();
  EXPECT_TRUE(queue.is_shutdown());
  EXPECT_EQ(queue.push(item(2)), PushResult::Shutdown);
  EXPECT_FALSE(queue.pop(kLongWait).has_value());  // returns at once; queued item left in place
  EXPECT_EQ(queue.size(), 1u);
}

TEST_F(BoundedQueueTest, ShutdownWakesManyWaiters) {
  Queue queue(1, blocking_);
  std::vector<std::thread> consumers;
  for (int i = 0; i < 4; ++i) consumers.emplace_back([&] { queue.pop(kLongWait); });
  let_other_thread_block();
  const auto start = Steady::now();
  queue.shutdown();
  for (std::thread& t : consumers) t.join();
  EXPECT_LT(Steady::now() - start, milliseconds(50));  // notify_all, not notify_one
}

// --- Producer/consumer stress (FR-Q-03, run under TSan) ---

TEST_F(BoundedQueueTest, ProducerConsumerKeepsFifoAndCapacity) {
  constexpr std::uint64_t kItems = 20000;
  Queue queue(4, blocking_);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) ASSERT_EQ(queue.push(item(i)), PushResult::Queued);
  });

  for (std::uint64_t expected = 0; expected < kItems; ++expected) {
    EXPECT_LE(queue.size(), 4u);
    std::optional<Item> popped = queue.pop(kLongWait);
    ASSERT_TRUE(popped.has_value());
    ASSERT_EQ(id_of(popped), expected);
  }
  producer.join();
  EXPECT_EQ(queue.size(), 0u);
}

TEST_F(BoundedQueueTest, DrainReturnsRemainingItemsInOrderAfterShutdown) {
  Queue queue(3, blocking_);
  for (std::uint64_t i = 0; i < 3; ++i) ASSERT_EQ(queue.push(item(i)), PushResult::Queued);
  queue.shutdown();
  std::deque<Item> rest = queue.drain();
  ASSERT_EQ(rest.size(), 3u);
  for (std::uint64_t i = 0; i < 3; ++i) EXPECT_EQ(rest[i]->meta.id, i);
  EXPECT_EQ(queue.size(), 0u);
}

// --- Depth recording (P1.6, FR-Q-07) ---

TEST_F(BoundedQueueTest, ReportsDepthAfterEveryPushAndPop) {
  std::vector<std::size_t> depths;
  Queue queue(3, blocking_, [&](std::size_t depth) { depths.push_back(depth); });
  ASSERT_EQ(queue.push(item(1)), PushResult::Queued);
  ASSERT_EQ(queue.push(item(2)), PushResult::Queued);
  ASSERT_TRUE(queue.pop(kNoWait).has_value());
  ASSERT_EQ(queue.push(item(3)), PushResult::Queued);
  ASSERT_TRUE(queue.pop(kNoWait).has_value());
  ASSERT_TRUE(queue.pop(kNoWait).has_value());
  EXPECT_EQ(depths, (std::vector<std::size_t>{1, 2, 1, 2, 1, 0}));
}

TEST_F(BoundedQueueTest, NoDepthSampleForTimedOutPopOrRejectedPush) {
  std::vector<std::size_t> depths;
  Queue queue(1, blocking_, [&](std::size_t depth) { depths.push_back(depth); });
  EXPECT_FALSE(queue.pop(kNoWait).has_value());  // timeout: depth unchanged
  queue.shutdown();
  EXPECT_EQ(queue.push(item(1)), PushResult::Shutdown);  // rejected: depth unchanged
  EXPECT_TRUE(depths.empty());
}

TEST_F(BoundedQueueTest, WorksWithoutDepthCallback) {
  Queue queue(1, blocking_);  // empty std::function must not be called
  ASSERT_EQ(queue.push(item(1)), PushResult::Queued);
  EXPECT_EQ(id_of(queue.pop(kNoWait)), 1u);
}

// The callback appends to a plain vector with no lock of its own: under TSan this proves the
// queue serialises callback calls from the producer and consumer threads, and the series is a
// valid walk (each sample differs from the previous one by exactly 1, never above capacity).
TEST_F(BoundedQueueTest, DepthCallbackIsSerialisedAcrossThreads) {
  constexpr std::uint64_t kItems = 5000;
  std::vector<std::size_t> depths;
  depths.reserve(2 * kItems);
  Queue queue(4, blocking_, [&](std::size_t depth) { depths.push_back(depth); });
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) ASSERT_EQ(queue.push(item(i)), PushResult::Queued);
  });
  for (std::uint64_t i = 0; i < kItems; ++i) ASSERT_TRUE(queue.pop(kLongWait).has_value());
  producer.join();

  ASSERT_EQ(depths.size(), 2u * kItems);
  std::size_t previous = 0;
  for (std::size_t depth : depths) {
    EXPECT_LE(depth, 4u);
    EXPECT_EQ(depth > previous ? depth - previous : previous - depth, 1u);
    previous = depth;
  }
  EXPECT_EQ(depths.back(), 0u);
}

// --- Policy delegation (P2.1, FR-Q-04) ---

// Records what the queue showed it, and answers with a scripted decision.
class SpyPolicy final : public OverloadPolicy {
 public:
  PushDecision on_push(const QueueView& queue, const FrameMeta& incoming) override {
    seen_sizes.push_back(queue.size);
    seen_ids.push_back(incoming.id);
    capacity = queue.capacity;
    return answer;
  }
  std::string_view name() const override { return "spy"; }

  std::atomic<PushDecision> answer{PushDecision::Insert};  // changed by the test thread
  std::vector<std::size_t> seen_sizes;
  std::vector<std::uint64_t> seen_ids;
  std::size_t capacity = 0;
};

TEST(BoundedQueuePolicyTest, AsksThePolicyOnEveryPushWithQueueStateAndIncomingMeta) {
  SpyPolicy spy;
  Queue queue(3, spy);
  ASSERT_EQ(queue.push(item(10)), PushResult::Queued);
  ASSERT_EQ(queue.push(item(11)), PushResult::Queued);
  EXPECT_EQ(spy.seen_sizes, (std::vector<std::size_t>{0, 1}));
  EXPECT_EQ(spy.seen_ids, (std::vector<std::uint64_t>{10, 11}));
  EXPECT_EQ(spy.capacity, 3u);
}

// A policy that says Insert into a full queue must not break FR-Q-01: the push waits instead.
TEST(BoundedQueuePolicyTest, InsertIntoFullQueueWaitsInsteadOfOverfilling) {
  SpyPolicy spy;  // always answers Insert
  Queue queue(1, spy);
  ASSERT_EQ(queue.push(item(1)), PushResult::Queued);
  std::atomic<bool> pushed{false};
  std::thread producer([&] {
    EXPECT_EQ(queue.push(item(2)), PushResult::Queued);
    pushed = true;
  });
  let_other_thread_block();
  EXPECT_FALSE(pushed);
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_EQ(id_of(queue.pop(kNoWait)), 1u);
  producer.join();
  EXPECT_EQ(id_of(queue.pop(kNoWait)), 2u);
}

TEST(BoundedQueuePolicyTest, WaitDecisionIsReAskedAfterSpaceFrees) {
  SpyPolicy spy;
  Queue queue(1, spy);
  ASSERT_EQ(queue.push(item(1)), PushResult::Queued);
  spy.answer = PushDecision::Wait;
  std::thread producer([&] { EXPECT_EQ(queue.push(item(2)), PushResult::Queued); });
  let_other_thread_block();
  spy.answer = PushDecision::Insert;  // the woken push asks again and now gets Insert
  EXPECT_EQ(id_of(queue.pop(kNoWait)), 1u);
  producer.join();
  EXPECT_EQ(spy.seen_ids.back(), 2u);
  EXPECT_GE(spy.seen_ids.size(), 3u);  // asked for item 1, then twice (or more) for item 2
}

}  // namespace
}  // namespace frameflow
