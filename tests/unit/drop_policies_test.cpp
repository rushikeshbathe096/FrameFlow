#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "frameflow/bounded_queue.hpp"
#include "frameflow/policy/drop_newest_policy.hpp"
#include "frameflow/policy/drop_oldest_policy.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;

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

struct Dropped {
  std::uint64_t id;
  DropReason reason;
  bool operator==(const Dropped& other) const { return id == other.id && reason == other.reason; }
};

// Collects drop callbacks and queue contents for assertions.
struct Recorder {
  std::vector<Dropped> drops;
  std::vector<std::size_t> depths;
  Queue::DepthCallback depth_cb() {
    return [this](std::size_t depth) { depths.push_back(depth); };
  }
  Queue::DropCallback drop_cb() {
    return [this](Item dropped) { drops.push_back({dropped->meta.id, dropped->meta.drop_reason}); };
  }
};

std::vector<std::uint64_t> pop_all(Queue& queue) {
  std::vector<std::uint64_t> ids;
  while (std::optional<Item> popped = queue.pop(milliseconds(0))) ids.push_back((*popped)->meta.id);
  return ids;
}

TEST(DropOldestPolicyTest, InsertsWhenSpaceAndDropsOldestWhenFull) {
  DropOldestPolicy policy;
  EXPECT_EQ(policy.on_push(QueueView{1, 2}, FrameMeta{}), PushDecision::Insert);
  EXPECT_EQ(policy.on_push(QueueView{2, 2}, FrameMeta{}), PushDecision::DropOldest);
  EXPECT_EQ(policy.name(), "drop-oldest");
}

TEST(DropNewestPolicyTest, InsertsWhenSpaceAndDropsIncomingWhenFull) {
  DropNewestPolicy policy;
  EXPECT_EQ(policy.on_push(QueueView{1, 2}, FrameMeta{}), PushDecision::Insert);
  EXPECT_EQ(policy.on_push(QueueView{2, 2}, FrameMeta{}), PushDecision::DropIncoming);
  EXPECT_EQ(policy.name(), "drop-newest");
}

// FR-POL-03: oldest id dropped with reason `oldest`, newest kept, push never blocks.
TEST(DropOldestQueueTest, KeepsNewestFramesAndRecordsOldestDrops) {
  DropOldestPolicy policy;
  Recorder rec;
  Queue queue(3, policy, rec.depth_cb(), rec.drop_cb());
  for (std::uint64_t id = 0; id < 5; ++id) EXPECT_EQ(queue.push(item(id)), PushResult::Queued);

  EXPECT_EQ(rec.drops, (std::vector<Dropped>{{0, DropReason::Oldest}, {1, DropReason::Oldest}}));
  EXPECT_EQ(rec.depths, (std::vector<std::size_t>{1, 2, 3, 3, 3}));  // never above capacity
  EXPECT_EQ(pop_all(queue), (std::vector<std::uint64_t>{2, 3, 4}));
}

// FR-POL-04: the incoming frame is dropped with reason `newest`; the queue is unchanged.
TEST(DropNewestQueueTest, KeepsQueueAndRecordsIncomingDrops) {
  DropNewestPolicy policy;
  Recorder rec;
  Queue queue(3, policy, rec.depth_cb(), rec.drop_cb());
  for (std::uint64_t id = 0; id < 3; ++id) EXPECT_EQ(queue.push(item(id)), PushResult::Queued);
  EXPECT_EQ(queue.push(item(3)), PushResult::Dropped);
  EXPECT_EQ(queue.push(item(4)), PushResult::Dropped);

  EXPECT_EQ(rec.drops, (std::vector<Dropped>{{3, DropReason::Newest}, {4, DropReason::Newest}}));
  EXPECT_EQ(pop_all(queue), (std::vector<std::uint64_t>{0, 1, 2}));
}

// EC-01: capacity 1 works for both policies.
TEST(DropOldestQueueTest, CapacityOneReplacesTheSingleQueuedFrame) {
  DropOldestPolicy policy;
  Recorder rec;
  Queue queue(1, policy, {}, rec.drop_cb());
  for (std::uint64_t id = 0; id < 4; ++id) EXPECT_EQ(queue.push(item(id)), PushResult::Queued);
  EXPECT_EQ(rec.drops.size(), 3u);
  EXPECT_EQ(pop_all(queue), (std::vector<std::uint64_t>{3}));
}

TEST(DropNewestQueueTest, CapacityOneKeepsTheFirstFrame) {
  DropNewestPolicy policy;
  Recorder rec;
  Queue queue(1, policy, {}, rec.drop_cb());
  EXPECT_EQ(queue.push(item(0)), PushResult::Queued);
  for (std::uint64_t id = 1; id < 4; ++id) EXPECT_EQ(queue.push(item(id)), PushResult::Dropped);
  EXPECT_EQ(rec.drops.size(), 3u);
  EXPECT_EQ(pop_all(queue), (std::vector<std::uint64_t>{0}));
}

TEST(DropPoliciesTest, DroppedItemsAreFreedWithoutACallback) {
  DropOldestPolicy oldest;
  Queue queue(1, oldest);  // ASan's leak check fails the run if a dropped item leaks
  for (std::uint64_t id = 0; id < 10; ++id) queue.push(item(id));
  EXPECT_EQ(queue.size(), 1u);
}

TEST(DropPoliciesTest, ShutdownStillRejectsPushes) {
  DropNewestPolicy policy;
  Queue queue(1, policy);
  queue.shutdown();
  EXPECT_EQ(queue.push(item(0)), PushResult::Shutdown);
}

// Fast producer, slow consumer, drop-oldest (run under TSan): every id is either popped or
// dropped exactly once, popped ids increase (BR-03), and the producer never blocks.
TEST(DropOldestQueueTest, ConcurrentProducerConsumerAccountsForEveryFrame) {
  constexpr std::uint64_t kItems = 3000;
  DropOldestPolicy policy;
  Recorder rec;  // written only from the drop callback, i.e. under the queue lock
  Queue queue(4, policy, {}, rec.drop_cb());
  std::thread producer([&] {
    for (std::uint64_t id = 0; id < kItems; ++id)
      ASSERT_EQ(queue.push(item(id)), PushResult::Queued);
    queue.shutdown();
  });
  std::vector<std::uint64_t> popped;
  while (true) {
    std::optional<Item> next = queue.pop(milliseconds(100));
    if (!next) {
      if (queue.is_shutdown()) break;
      continue;
    }
    popped.push_back((*next)->meta.id);
  }
  producer.join();
  const std::size_t leftover = queue.drain().size();

  EXPECT_EQ(popped.size() + rec.drops.size() + leftover, kItems);
  for (std::size_t i = 1; i < popped.size(); ++i) EXPECT_LT(popped[i - 1], popped[i]);
  for (const Dropped& d : rec.drops) EXPECT_EQ(d.reason, DropReason::Oldest);
}

}  // namespace
}  // namespace frameflow
