#include "frameflow/source/synthetic_source.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace frameflow {

namespace {
constexpr int kBarWidth = 16;
constexpr std::uint64_t kBarStep = 8;  // px per frame
}  // namespace

SyntheticSource::SyntheticSource(cv::Size resolution) : resolution_(resolution) {
  if (resolution.width <= 0 || resolution.height <= 0) {
    throw std::invalid_argument("SyntheticSource: resolution must be positive");
  }
}

FramePtr SyntheticSource::next(std::uint64_t id) {
  const cv::Scalar background(static_cast<double>(id % 256), static_cast<double>(id * 3 % 256),
                              static_cast<double>(id * 7 % 256));
  cv::Mat image(resolution_, CV_8UC3, background);

  const auto width = static_cast<std::uint64_t>(resolution_.width);
  const int x = static_cast<int>(id * kBarStep % width);
  const int bar = std::min(kBarWidth, resolution_.width - x);
  image(cv::Rect(x, 0, bar, resolution_.height)).setTo(cv::Scalar(255, 255, 255));

  return std::make_unique<Frame>(id, std::move(image));
}

}  // namespace frameflow
