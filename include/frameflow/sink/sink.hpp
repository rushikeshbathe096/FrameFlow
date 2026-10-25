// sink.hpp
// Responsibility: interface for the final pipeline stage. consume() stamps the frame's t_out
// (FR-PROC-06) and may display it. The worker keeps ownership of the frame and records it after
// consume() returns (D16).
// Thread-safety: consume() is called by the worker thread only.
#pragma once

#include "frameflow/frame.hpp"

namespace frameflow {

class ISink {
 public:
  virtual ~ISink() = default;
  virtual void consume(Frame& frame) = 0;
};

}  // namespace frameflow
