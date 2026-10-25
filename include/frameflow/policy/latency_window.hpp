// latency_window.hpp
// Responsibility: the adaptive policy's latency estimate L (FR-POL-05): the nearest-rank p95
// (FR-MET-03) of the last W delivered-frame latencies, kept in a fixed ring buffer that is
// allocated once. With fewer than W samples it uses all of them; with none there is no
// estimate (EC-14).
// Thread-safety: not thread-safe. AdaptivePolicy owns one and guards it with its own mutex.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

#include "frameflow/clock.hpp"

namespace frameflow {

class LatencyWindow {
 public:
  explicit LatencyWindow(std::size_t capacity) : samples_(capacity) {
    if (capacity == 0) throw std::invalid_argument("LatencyWindow: capacity must be at least 1");
  }

  // Adds a sample; once full, it overwrites the oldest one.
  void add(Clock::duration latency) {
    samples_[next_] = latency;
    next_ = (next_ + 1) % samples_.size();
    count_ = std::min(count_ + 1, samples_.size());
  }

  std::size_t size() const { return count_; }

  // Nearest-rank p95 of the samples in the window; std::nullopt when empty.
  std::optional<Clock::duration> p95() const {
    if (count_ == 0) return std::nullopt;
    // The first count_ slots hold the samples until the buffer wraps; after that all do.
    std::vector<Clock::duration> sorted(samples_.begin(),
                                        samples_.begin() + static_cast<std::ptrdiff_t>(count_));
    const std::size_t rank = (95 * count_ + 99) / 100;  // ceil(0.95 n) in integers, >= 1
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(rank - 1),
                     sorted.end());
    return sorted[rank - 1];
  }

 private:
  std::vector<Clock::duration> samples_;  // ring buffer, size = W
  std::size_t next_ = 0;                  // slot the next sample goes into
  std::size_t count_ = 0;                 // samples held, at most W
};

}  // namespace frameflow
