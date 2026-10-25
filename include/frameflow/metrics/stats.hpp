// stats.hpp
// Responsibility: summary statistics of one finished run (FR-MET-02, FR-MET-04): nearest-rank
// latency percentiles over delivered frames (FR-MET-03, BR-05), input/output FPS, drop rate overall
// and per reason, and queue-depth mean/max. Warm-up frames are excluded from rates and percentiles
// but not from frame counts (BR-06, D19). A value that cannot be computed (no delivered frames,
// empty window) is std::nullopt, never 0 (EC-15).
// Thread-safety: pure functions; run on the main thread after the pipeline has joined.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"

namespace frameflow {

// Nearest-rank percentile (FR-MET-03): the value at index ceil(p / 100 x n) - 1 of the sorted
// values, for 0 < p <= 100. std::nullopt for empty input.
std::optional<double> percentile(std::vector<double> values, double p);

struct RunStats {
  // Whole run (BR-01).
  std::uint64_t produced = 0;
  std::uint64_t delivered = 0;
  std::uint64_t dropped = 0;
  std::uint64_t in_flight = 0;

  // Measurement window [t0 + warmup, t_end) only (BR-06).
  double window_s = 0.0;
  std::uint64_t window_produced = 0;
  std::uint64_t window_delivered = 0;
  std::uint64_t window_dropped_oldest = 0;
  std::uint64_t window_dropped_newest = 0;
  std::uint64_t window_dropped_stale = 0;
  std::optional<double> p50_ms, p95_ms, p99_ms, max_ms;
  std::optional<double> input_fps, output_fps;
  std::optional<double> drop_rate_pct;  // all reasons
  std::optional<double> drop_oldest_pct, drop_newest_pct, drop_stale_pct;
  std::optional<double> depth_mean;
  std::optional<std::size_t> depth_max;
};

RunStats compute_run_stats(const std::vector<FrameMeta>& frames,
                           const std::vector<DepthSample>& depth, Clock::time_point t0,
                           Clock::time_point t_end, Clock::duration warmup);

}  // namespace frameflow
