// null_sink.hpp
// Responsibility: sink that only stamps t_out; used for measurements (no display cost).
// Thread-safety: called by the worker thread only; the Clock must be thread-safe.
#pragma once

#include "frameflow/clock.hpp"
#include "frameflow/sink/sink.hpp"

namespace frameflow {

class NullSink final : public ISink {
 public:
  explicit NullSink(Clock& clock) : clock_(clock) {}
  void consume(Frame& frame) override { frame.meta.t_out = clock_.now(); }

 private:
  Clock& clock_;
};

}  // namespace frameflow
