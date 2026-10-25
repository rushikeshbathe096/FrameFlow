#include "frameflow/processor/image_processors.hpp"

#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>

namespace frameflow {

namespace {

cv::Mat to_grey(const cv::Mat& image) {
  if (image.channels() == 1) return image;
  cv::Mat grey;
  cv::cvtColor(image, grey, image.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
  return grey;
}

}  // namespace

void GrayscaleProcessor::process(cv::Mat& image) const { image = to_grey(image); }

BlurProcessor::BlurProcessor(int kernel) : kernel_(kernel) {
  if (kernel < 3 || kernel > 99 || kernel % 2 == 0) {
    throw std::invalid_argument("blur kernel must be an odd number from 3 to 99, got " +
                                std::to_string(kernel));
  }
}

void BlurProcessor::process(cv::Mat& image) const {
  cv::Mat blurred;
  cv::GaussianBlur(image, blurred, cv::Size(kernel_, kernel_), 0);
  image = blurred;
}

void CannyProcessor::process(cv::Mat& image) const {
  cv::Mat edges;
  cv::Canny(to_grey(image), edges, kLowThreshold, kHighThreshold);
  image = edges;
}

}  // namespace frameflow
