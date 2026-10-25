#include "frameflow/frame.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

namespace frameflow {
namespace {

// Move-only, and moves cannot throw (so containers like std::deque move instead of copying).
static_assert(!std::is_copy_constructible_v<Frame>);
static_assert(!std::is_copy_assignable_v<Frame>);
static_assert(std::is_nothrow_move_constructible_v<Frame>);
static_assert(std::is_nothrow_move_assignable_v<Frame>);
static_assert(!std::is_copy_constructible_v<FramePtr>);

cv::Mat make_image() { return cv::Mat(4, 6, CV_8UC3, cv::Scalar(1, 2, 3)); }

TEST(FrameMetaTest, DefaultsAreEmpty) {
  const FrameMeta meta;
  EXPECT_EQ(meta.id, 0u);
  EXPECT_FALSE(meta.t_cap.has_value());
  EXPECT_FALSE(meta.t_push.has_value());
  EXPECT_FALSE(meta.t_pop.has_value());
  EXPECT_FALSE(meta.t_proc_start.has_value());
  EXPECT_FALSE(meta.t_proc_end.has_value());
  EXPECT_FALSE(meta.t_out.has_value());
  EXPECT_EQ(meta.source_lateness, Clock::duration::zero());
  EXPECT_EQ(meta.drop_reason, DropReason::None);
  EXPECT_FALSE(meta.is_dropped());
  EXPECT_FALSE(meta.warmup);
}

TEST(FrameMetaTest, TimestampAtClockZeroIsStillSet) {
  // Why optional: FakeClock starts at time_point{}, which must not read as "not reached".
  FrameMeta meta;
  meta.t_cap = Clock::time_point{};
  EXPECT_TRUE(meta.t_cap.has_value());
}

TEST(FrameMetaTest, IsDroppedFollowsDropReason) {
  FrameMeta meta;
  for (DropReason reason : {DropReason::Oldest, DropReason::Newest, DropReason::Stale}) {
    meta.drop_reason = reason;
    EXPECT_TRUE(meta.is_dropped());
  }
  meta.drop_reason = DropReason::None;
  EXPECT_FALSE(meta.is_dropped());
}

TEST(FrameMetaTest, DropReasonNamesMatchResultFiles) {
  EXPECT_EQ(drop_reason_name(DropReason::None), "");
  EXPECT_EQ(drop_reason_name(DropReason::Oldest), "oldest");
  EXPECT_EQ(drop_reason_name(DropReason::Newest), "newest");
  EXPECT_EQ(drop_reason_name(DropReason::Stale), "stale");
}

TEST(FrameTest, ConstructorSetsIdAndKeepsPixelBuffer) {
  cv::Mat image = make_image();
  const uchar* pixels = image.data;
  const Frame frame(7, image);
  EXPECT_EQ(frame.meta.id, 7u);
  EXPECT_EQ(frame.image.data, pixels);  // shared buffer, no deep copy
  EXPECT_EQ(frame.image.rows, 4);
  EXPECT_EQ(frame.image.cols, 6);
}

TEST(FrameTest, MoveConstructionTransfersPixelsAndMeta) {
  Frame source(3, make_image());
  source.meta.t_cap = Clock::time_point{} + std::chrono::milliseconds(33);
  const uchar* pixels = source.image.data;

  const Frame target(std::move(source));

  EXPECT_EQ(target.image.data, pixels);
  EXPECT_EQ(target.meta.id, 3u);
  EXPECT_EQ(target.meta.t_cap, Clock::time_point{} + std::chrono::milliseconds(33));
  EXPECT_TRUE(source.image.empty());  // NOLINT(bugprone-use-after-move): checking moved-from state
}

TEST(FrameTest, MoveAssignmentTransfersPixels) {
  Frame source(1, make_image());
  Frame target(2, cv::Mat());
  const uchar* pixels = source.image.data;

  target = std::move(source);

  EXPECT_EQ(target.image.data, pixels);
  EXPECT_EQ(target.meta.id, 1u);
}

TEST(FramePtrTest, MoveTransfersOwnershipAndNullsSource) {
  FramePtr first = std::make_unique<Frame>(5, make_image());
  Frame* raw = first.get();

  FramePtr second = std::move(first);

  EXPECT_EQ(first, nullptr);  // NOLINT(bugprone-use-after-move): checking moved-from state
  EXPECT_EQ(second.get(), raw);
  EXPECT_EQ(second->meta.id, 5u);
}

}  // namespace
}  // namespace frameflow
