#include "frameflow/metrics/metrics_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace frameflow {
namespace {

FrameMeta meta_with_id(std::uint64_t id) {
  FrameMeta meta;
  meta.id = id;
  return meta;
}

TEST(FrameCapacityTest, AddsTenPercentHeadroom) {
  EXPECT_EQ(frame_capacity_for(30.0, 60.0), 1980u);  // 30 x 60 x 1.1
  EXPECT_EQ(frame_capacity_for(30.0, 1.0), 33u);
  EXPECT_EQ(frame_capacity_for(25.0, 1.0), 28u);  // 27.5 rounds up
}

TEST(MetricsStoreTest, StartsEmpty) {
  MetricsStore store(10);
  EXPECT_EQ(store.frame_capacity(), 10u);
  EXPECT_TRUE(store.frames().empty());
  EXPECT_TRUE(store.depth_samples().empty());
}

TEST(MetricsStoreTest, ReturnsRecordedFramesInIdOrder) {
  MetricsStore store(10);
  for (std::uint64_t id : {5u, 1u, 3u}) store.record_frame(meta_with_id(id));
  const std::vector<FrameMeta> frames = store.frames();
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0].id, 1u);
  EXPECT_EQ(frames[1].id, 3u);
  EXPECT_EQ(frames[2].id, 5u);
}

TEST(MetricsStoreTest, KeepsAllFields) {
  MetricsStore store(2);
  FrameMeta meta = meta_with_id(1);
  meta.t_cap = Clock::time_point{} + std::chrono::milliseconds(33);
  meta.drop_reason = DropReason::Oldest;
  meta.source_lateness = std::chrono::microseconds(250);
  store.record_frame(meta);
  const FrameMeta stored = store.frames().at(0);
  EXPECT_EQ(stored.t_cap, meta.t_cap);
  EXPECT_EQ(stored.drop_reason, DropReason::Oldest);
  EXPECT_EQ(stored.source_lateness, std::chrono::microseconds(250));
}

TEST(MetricsStoreTest, RejectsIdBeyondCapacity) {
  MetricsStore store(3);
  EXPECT_THROW(store.record_frame(meta_with_id(3)), std::out_of_range);
}

TEST(MetricsStoreTest, RejectsSecondRecordOfSameId) {
  MetricsStore store(3);
  store.record_frame(meta_with_id(2));
  EXPECT_THROW(store.record_frame(meta_with_id(2)), std::logic_error);
}

TEST(MetricsStoreTest, FillingToCapacityNeverReallocates) {
  constexpr std::size_t kCapacity = 100;
  MetricsStore store(kCapacity);
  const DepthSample* depth_before = store.depth_samples().data();
  for (std::uint64_t id = 0; id < kCapacity; ++id) {
    store.record_frame(meta_with_id(id));
    store.record_depth(Clock::time_point{}, 1);  // push
    store.record_depth(Clock::time_point{}, 0);  // pop
  }
  EXPECT_EQ(store.depth_samples().data(), depth_before);  // DR-09: no reallocation
  EXPECT_EQ(store.depth_samples().size(), 2 * kCapacity);
  EXPECT_EQ(store.frames().size(), kCapacity);
}

TEST(MetricsStoreTest, DepthSamplesKeepOrderAndValues) {
  MetricsStore store(4);
  store.record_depth(Clock::time_point{} + std::chrono::milliseconds(1), 1);
  store.record_depth(Clock::time_point{} + std::chrono::milliseconds(2), 2);
  ASSERT_EQ(store.depth_samples().size(), 2u);
  EXPECT_EQ(store.depth_samples()[1].depth, 2u);
  EXPECT_EQ(store.depth_samples()[1].t, Clock::time_point{} + std::chrono::milliseconds(2));
}

// Two threads record disjoint ids at the same time (source drops vs worker deliveries); under
// TSan this proves that per-slot writes need no lock.
TEST(MetricsStoreTest, ConcurrentRecordsOfDistinctIdsAreSafe) {
  constexpr std::uint64_t kPerThread = 5000;
  MetricsStore store(2 * kPerThread);
  std::thread even([&] {
    for (std::uint64_t i = 0; i < kPerThread; ++i) store.record_frame(meta_with_id(2 * i));
  });
  std::thread odd([&] {
    for (std::uint64_t i = 0; i < kPerThread; ++i) store.record_frame(meta_with_id(2 * i + 1));
  });
  even.join();
  odd.join();
  const std::vector<FrameMeta> frames = store.frames();
  ASSERT_EQ(frames.size(), 2 * kPerThread);
  for (std::uint64_t id = 0; id < frames.size(); ++id) EXPECT_EQ(frames[id].id, id);
}

}  // namespace
}  // namespace frameflow
