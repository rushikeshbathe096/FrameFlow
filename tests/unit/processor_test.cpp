#include "frameflow/processor/processor.hpp"

#include <gtest/gtest.h>

#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>

#include "frameflow/processor/image_processors.hpp"

namespace frameflow {
namespace {

// 64x48 BGR: left half black, right half white, so blur and edges have something to act on.
cv::Mat half_and_half() {
  cv::Mat image(48, 64, CV_8UC3, cv::Scalar::all(0));
  image(cv::Rect(32, 0, 32, 48)).setTo(cv::Scalar::all(255));
  return image;
}

cv::Mat run(std::string_view name, cv::Mat image, int kernel = 15) {
  cv::setNumThreads(1);  // as Pipeline does (D50): the call runs on this thread only
  make_processor(name, kernel)->process(image);
  return image;
}

TEST(ProcessorTest, NamesInDisplayOrder) {
  EXPECT_EQ(processor_names(),
            (std::vector<std::string_view>{"none", "grayscale", "blur", "canny"}));
  for (std::string_view name : processor_names()) EXPECT_EQ(make_processor(name)->name(), name);
}

TEST(ProcessorTest, NoneLeavesTheImageUnchanged) {
  const cv::Mat input = half_and_half();
  const cv::Mat output = run("none", input.clone());
  EXPECT_EQ(cv::norm(input, output, cv::NORM_INF), 0.0);
}

TEST(ProcessorTest, GrayscaleGivesOneChannelOfTheSameSize) {
  const cv::Mat output = run("grayscale", half_and_half());
  EXPECT_EQ(output.size(), cv::Size(64, 48));
  EXPECT_EQ(output.type(), CV_8UC1);
  EXPECT_EQ(output.at<std::uint8_t>(10, 5), 0);
  EXPECT_EQ(output.at<std::uint8_t>(10, 60), 255);
  EXPECT_EQ(run("grayscale", output.clone()).type(), CV_8UC1);  // already grey: unchanged
}

TEST(ProcessorTest, BlurKeepsSizeAndTypeAndSoftensTheEdge) {
  const cv::Mat output = run("blur", half_and_half(), 15);
  EXPECT_EQ(output.size(), cv::Size(64, 48));
  EXPECT_EQ(output.type(), CV_8UC3);
  const int at_edge = output.at<cv::Vec3b>(24, 32)[0];
  EXPECT_GT(at_edge, 0);
  EXPECT_LT(at_edge, 255);                       // a hard edge became a ramp
  EXPECT_EQ(output.at<cv::Vec3b>(24, 2)[0], 0);  // far from the edge: unchanged
}

TEST(ProcessorTest, CannyGivesBinaryEdgesOfOneChannel) {
  const cv::Mat output = run("canny", half_and_half());
  EXPECT_EQ(output.size(), cv::Size(64, 48));
  EXPECT_EQ(output.type(), CV_8UC1);
  double min = 0;
  double max = 0;
  cv::minMaxLoc(output, &min, &max);
  EXPECT_EQ(min, 0.0);
  EXPECT_EQ(max, 255.0);
  EXPECT_GT(cv::countNonZero(output.col(31)) + cv::countNonZero(output.col(32)), 40);  // the edge
  EXPECT_EQ(cv::countNonZero(output.col(5)), 0);
}

// VR-12: odd, 3 to 99.
TEST(ProcessorTest, BlurKernelMustBeOddAndInRange) {
  for (int kernel : {3, 15, 99}) EXPECT_NO_THROW(BlurProcessor{kernel}) << kernel;
  for (int kernel : {1, 2, 14, 100, 101, -3}) {
    EXPECT_THROW(BlurProcessor{kernel}, std::invalid_argument) << kernel;
  }
}

TEST(ProcessorTest, UnknownNameListsValidNames) {
  try {
    make_processor("sharpen");
    FAIL() << "expected std::invalid_argument";
  } catch (const std::invalid_argument& e) {
    EXPECT_EQ(std::string(e.what()),
              "unknown processor 'sharpen' (valid: none, grayscale, blur, canny)");
  }
}

}  // namespace
}  // namespace frameflow
