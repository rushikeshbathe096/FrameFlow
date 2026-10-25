// metrics_store.hpp
// Responsibility: in-memory measurements of one run. One preallocated record per frame, indexed
// by frame id (FR-MET-01, DR-09), plus a preallocated queue-depth series. Nothing here does I/O;
// reporters read the store on the main thread after the pipeline threads have joined (AD-6).
// Thread-safety: record_frame() may be called concurrently from several threads as long as each
// frame id is recorded exactly once (each call writes only its own slot). record_depth() is not
// synchronised: callers must serialise it (the queue calls it under its own lock). Readers must
// run after all writers have finished (e.g. after the threads are joined), except live(), which
// reads only atomics and may be called from any thread at any time (`--progress`, D52).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"

namespace frameflow {

struct DepthSample {
  Clock::time_point t;
  std::size_t depth = 0;
};

// Expected frame count for a run, with 10% headroom (DR-09): ceil(fps x duration_s x 1.1).
std::size_t frame_capacity_for(double fps, double duration_s);

// Running totals for `--progress`, readable while the pipeline runs.
struct LiveCounts {
  std::uint64_t delivered = 0;
  std::uint64_t dropped = 0;
  std::size_t depth = 0;                        // latest queue depth sample
  std::optional<Clock::duration> last_latency;  // of the most recently recorded delivered frame
};

class MetricsStore {
 public:
  explicit MetricsStore(std::size_t frame_capacity);

  // Stores the final metadata of one frame. Throws std::out_of_range if meta.id >= capacity and
  // std::logic_error if that id was already recorded.
  void record_frame(const FrameMeta& meta);

  // Appends one queue-depth sample (FR-Q-07). Caller must serialise calls.
  void record_depth(Clock::time_point t, std::size_t depth);

  // Recorded frames in id order.
  std::vector<FrameMeta> frames() const;
  const std::vector<DepthSample>& depth_samples() const { return depth_; }
  std::size_t frame_capacity() const { return frames_.size(); }
  LiveCounts live() const;  // any thread

 private:
  std::vector<FrameMeta> frames_;
  std::vector<std::uint8_t> recorded_;  // not vector<bool>: its bits share bytes across ids
  std::vector<DepthSample> depth_;

  // Written by the recording threads, read by live(): plain counters, no ordering needed.
  std::atomic<std::uint64_t> live_delivered_{0};
  std::atomic<std::uint64_t> live_dropped_{0};
  std::atomic<std::size_t> live_depth_{0};
  std::atomic<Clock::duration::rep> live_latency_ticks_{-1};  // -1: nothing delivered yet
};

}  // namespace frameflow
