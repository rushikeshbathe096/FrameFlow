#include "frameflow/source/synthetic_source.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace frameflow {
namespace {

bool same_pixels(const cv::Mat& a, const cv::Mat& b) {
  return a.size() == b.size() && a.type() == b.type() && cv::norm(a, b, cv::NORM_INF) == 0.0;
}

TEST(SyntheticSourceTest, ProducesRequestedSizeAndType) {
  SyntheticSource source(cv::Size(320, 240));
  FramePtr frame = source.next(0);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->image.cols, 320);
  EXPECT_EQ(frame->image.rows, 240);
  EXPECT_EQ(frame->image.type(), CV_8UC3);
}

TEST(SyntheticSourceTest, SetsTheGivenId) {
  SyntheticSource source(cv::Size(64, 32));
  for (std::uint64_t id : {0u, 1u, 2u, 1000u}) EXPECT_EQ(source.next(id)->meta.id, id);
}

TEST(SyntheticSourceTest, SameIdGivesIdenticalPixels) {
  SyntheticSource a(cv::Size(128, 72));
  SyntheticSource b(cv::Size(128, 72));
  for (std::uint64_t id : {0u, 5u, 17u, 4242u}) {
    EXPECT_TRUE(same_pixels(a.next(id)->image, b.next(id)->image)) << "id " << id;
  }
}

TEST(SyntheticSourceTest, DifferentIdsGiveDifferentPixels) {
  SyntheticSource source(cv::Size(128, 72));
  EXPECT_FALSE(same_pixels(source.next(1)->image, source.next(2)->image));
}

TEST(SyntheticSourceTest, EachFrameOwnsItsOwnPixelBuffer) {
  SyntheticSource source(cv::Size(32, 16));
  FramePtr first = source.next(0);
  FramePtr second = source.next(0);
  EXPECT_NE(first->image.data, second->image.data);  // no shared buffers between frames
}

TEST(SyntheticSourceTest, BarWrapsAtSmallestResolution) {
  SyntheticSource source(cv::Size(16, 16));  // VR-13 minimum
  for (std::uint64_t id = 0; id < 10; ++id) {
    FramePtr frame = source.next(id);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->image.cols, 16);
  }
}

TEST(SyntheticSourceTest, RejectsEmptyResolution) {
  EXPECT_THROW(SyntheticSource(cv::Size(0, 10)), std::invalid_argument);
}

}  // namespace
}  // namespace frameflow
