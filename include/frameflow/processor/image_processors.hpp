// image_processors.hpp
// Responsibility: the four processors of FR-PROC-01 (see processor.hpp). Each is a small
// stateless wrapper around one OpenCV call.
// Thread-safety: process() is const and keeps no state; used by the worker thread only.
#pragma once

#include "frameflow/processor/processor.hpp"

namespace frameflow {

class NoneProcessor final : public IProcessor {
 public:
  void process(cv::Mat& /*image*/) const override {}
  std::string_view name() const override { return "none"; }
};

class GrayscaleProcessor final : public IProcessor {
 public:
  void process(cv::Mat& image) const override;  // BGR (or grey) -> CV_8UC1
  std::string_view name() const override { return "grayscale"; }
};

class BlurProcessor final : public IProcessor {
 public:
  explicit BlurProcessor(int kernel);           // odd, 3-99 (VR-12); throws std::invalid_argument
  void process(cv::Mat& image) const override;  // Gaussian, same size and type
  std::string_view name() const override { return "blur"; }

 private:
  int kernel_;
};

class CannyProcessor final : public IProcessor {
 public:
  static constexpr double kLowThreshold = 50.0;
  static constexpr double kHighThreshold = 150.0;
  void process(cv::Mat& image) const override;  // grey, then Canny edges: CV_8UC1 of 0/255
  std::string_view name() const override { return "canny"; }
};

}  // namespace frameflow
