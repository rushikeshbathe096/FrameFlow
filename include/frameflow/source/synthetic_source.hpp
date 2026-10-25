// synthetic_source.hpp
// Responsibility: generates BGR frames of a fixed resolution whose pixels depend only on the
// frame id (FR-SRC-02): a background colour derived from the id plus a white vertical bar that
// moves 8 px per frame. The same id always gives identical pixels, so runs are repeatable.
// Thread-safety: none; used by the source thread only.
#pragma once

#include <opencv2/core.hpp>

#include "frameflow/source/source.hpp"

namespace frameflow {

class SyntheticSource final : public ISource {
 public:
  explicit SyntheticSource(cv::Size resolution);

  FramePtr next(std::uint64_t id) override;  // never ends: never returns nullptr

 private:
  cv::Size resolution_;
};

}  // namespace frameflow
