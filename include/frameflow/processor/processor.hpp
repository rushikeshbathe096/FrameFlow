// processor.hpp
// Responsibility: the image-processing step of the worker (FR-PROC-01). IProcessor transforms a
// frame's image; the four processors are `none` (leaves it unchanged), `grayscale` (BGR -> one
// channel), `blur` (Gaussian, odd kernel `--kernel`) and `canny` (edges of the grey image, one
// channel). New processors need one class and one line in processor_factory.cpp (RR-06).
// Thread-safety: a processor is created on the main thread and then used by the worker thread
// only. Pipeline turns OpenCV's own thread pool off (cv::setNumThreads(1), D50), so each call
// runs entirely on the worker thread.
#pragma once

#include <memory>
#include <opencv2/core.hpp>
#include <string_view>
#include <vector>

namespace frameflow {

class IProcessor {
 public:
  virtual ~IProcessor() = default;
  virtual void process(cv::Mat& image) const = 0;  // replaces `image` with the result
  virtual std::string_view name() const = 0;
};

// Throws std::invalid_argument for an unknown name or a kernel that is not odd and in 3-99.
std::unique_ptr<IProcessor> make_processor(std::string_view name, int kernel = 15);

// Registered names in display order: none, grayscale, blur, canny.
std::vector<std::string_view> processor_names();

}  // namespace frameflow
