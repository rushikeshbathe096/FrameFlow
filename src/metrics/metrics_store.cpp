#include "frameflow/metrics/metrics_store.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace frameflow {

std::size_t frame_capacity_for(double fps, double duration_s) {
  // The epsilon absorbs binary rounding (30 x 60 x 1.1 is 1980.0000000000002 in double), so an
  // exact product is not rounded up to the next frame.
  return static_cast<std::size_t>(std::ceil(fps * duration_s * 1.1 - 1e-9));
}

MetricsStore::MetricsStore(std::size_t frame_capacity)
    : frames_(frame_capacity), recorded_(frame_capacity, 0) {
  depth_.reserve(2 * frame_capacity + 2);  // one sample per push and per pop
}

void MetricsStore::record_frame(const FrameMeta& meta) {
  if (meta.id >= frames_.size()) {
    throw std::out_of_range("MetricsStore: frame id " + std::to_string(meta.id) +
                            " exceeds capacity " + std::to_string(frames_.size()));
  }
  if (recorded_[meta.id] != 0) {
    throw std::logic_error("MetricsStore: frame id " + std::to_string(meta.id) + " recorded twice");
  }
  frames_[meta.id] = meta;
  recorded_[meta.id] = 1;
}

void MetricsStore::record_depth(Clock::time_point t, std::size_t depth) {
  depth_.push_back(DepthSample{t, depth});
}

std::vector<FrameMeta> MetricsStore::frames() const {
  std::vector<FrameMeta> out;
  for (std::size_t id = 0; id < frames_.size(); ++id) {
    if (recorded_[id] != 0) out.push_back(frames_[id]);
  }
  return out;
}

}  // namespace frameflow
