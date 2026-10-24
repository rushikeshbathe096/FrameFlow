// Smoke test: proves OpenCV headers and libraries are found and linked correctly.
#include <gtest/gtest.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

TEST(OpenCvSmokeTest, GaussianBlurOn64x64Mat) {
  cv::Mat input(64, 64, CV_8UC1, cv::Scalar(0));
  input.at<uchar>(32, 32) = 255;  // single bright pixel; blur should spread it

  cv::Mat output;
  cv::GaussianBlur(input, output, cv::Size(5, 5), 1.0);

  EXPECT_EQ(output.rows, 64);
  EXPECT_EQ(output.cols, 64);
  EXPECT_EQ(output.type(), CV_8UC1);
  EXPECT_LT(output.at<uchar>(32, 32), 255);  // peak was spread out
  EXPECT_GT(output.at<uchar>(32, 33), 0);    // neighbour received some of it
}
