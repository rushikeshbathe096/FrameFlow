#include "frameflow/policy/latency_window.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace frameflow {
namespace {

using std::chrono::milliseconds;

// EC-14: before anything is delivered there is no estimate (not 0).
TEST(LatencyWindowTest, EmptyHasNoEstimate) {
  const LatencyWindow window(30);
  EXPECT_EQ(window.size(), 0u);
  EXPECT_FALSE(window.p95().has_value());
}

// EC-14: fewer samples than W use all of them.
TEST(LatencyWindowTest, FewerThanWindowUsesAllSamples) {
  LatencyWindow window(30);
  window.add(milliseconds(10));
  EXPECT_EQ(window.p95(), milliseconds(10));
  window.add(milliseconds(50));
  window.add(milliseconds(20));
  EXPECT_EQ(window.size(), 3u);
  EXPECT_EQ(window.p95(), milliseconds(50));  // rank ceil(0.95 x 3) = 3: the largest
}

// FR-POL-05 + FR-MET-03: nearest rank, the value at index ceil(0.95 n) - 1 of the sorted list.
TEST(LatencyWindowTest, NearestRankP95) {
  LatencyWindow window(100);
  for (int ms = 100; ms >= 1; --ms) window.add(milliseconds(ms));  // insertion order irrelevant
  EXPECT_EQ(window.p95(), milliseconds(95));
  LatencyWindow twenty(20);
  for (int ms = 1; ms <= 20; ++ms) twenty.add(milliseconds(ms));
  EXPECT_EQ(twenty.p95(), milliseconds(19));  // ceil(0.95 x 20) = 19
}

// The window slides: only the last W samples count.
TEST(LatencyWindowTest, OldSamplesLeaveTheWindow) {
  LatencyWindow window(5);
  for (int i = 0; i < 5; ++i) window.add(milliseconds(500));  // a spike...
  EXPECT_EQ(window.p95(), milliseconds(500));
  for (int i = 0; i < 4; ++i) window.add(milliseconds(10));
  EXPECT_EQ(window.p95(), milliseconds(500));  // one spike sample still inside
  window.add(milliseconds(10));
  EXPECT_EQ(window.size(), 5u);
  EXPECT_EQ(window.p95(), milliseconds(10));  // ...has fully left after W new samples
}

TEST(LatencyWindowTest, WrapsAroundManyTimes) {
  LatencyWindow window(7);
  for (int i = 0; i < 1000; ++i) window.add(milliseconds(i));
  EXPECT_EQ(window.size(), 7u);
  EXPECT_EQ(window.p95(), milliseconds(999));  // last 7 are 993..999
}

TEST(LatencyWindowTest, WindowOfOne) {
  LatencyWindow window(1);
  window.add(milliseconds(3));
  window.add(milliseconds(8));
  EXPECT_EQ(window.p95(), milliseconds(8));
}

TEST(LatencyWindowTest, ZeroCapacityIsRejected) {
  EXPECT_THROW(LatencyWindow(0), std::invalid_argument);
}

}  // namespace
}  // namespace frameflow
