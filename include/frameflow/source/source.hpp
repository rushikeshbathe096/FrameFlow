// source.hpp
// Responsibility: interface for frame producers (synthetic generator, video file). A source only
// creates frames; pacing, ids and timestamps are handled by SourceRunner.
// Thread-safety: an ISource is used by one thread (the source thread) only.
#pragma once

#include <cstdint>

#include "frameflow/frame.hpp"

namespace frameflow {

class ISource {
 public:
  virtual ~ISource() = default;

  // Returns the next frame with meta.id == id, or nullptr when the input has ended.
  virtual FramePtr next(std::uint64_t id) = 0;
};

}  // namespace frameflow
