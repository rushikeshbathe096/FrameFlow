#include "frameflow/metrics/stats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace frameflow {

namespace {

std::optional<double> rate_pct(std::uint64_t part, std::uint64_t whole) {
  if (whole == 0) return std::nullopt;
  return 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

std::optional<double> per_second(std::uint64_t count, double window_s) {
  if (window_s <= 0.0) return std::nullopt;
  return static_cast<double>(count) / window_s;
}

void count_whole_run(const FrameMeta& meta, RunStats& stats) {
  ++stats.produced;
  if (meta.is_dropped()) {
    ++stats.dropped;
  } else if (meta.t_out) {
    ++stats.delivered;
  } else {
    ++stats.in_flight;
  }
}

void count_window(const FrameMeta& meta, RunStats& stats, std::vector<double>& latencies_ms) {
  ++stats.window_produced;
  switch (meta.drop_reason) {
    case DropReason::Oldest:
      ++stats.window_dropped_oldest;
      return;
    case DropReason::Newest:
      ++stats.window_dropped_newest;
      return;
    case DropReason::Stale:
      ++stats.window_dropped_stale;
      return;
    case DropReason::None:
      break;
  }
  if (!meta.t_out || !meta.t_cap) return;  // in flight
  ++stats.window_delivered;
  latencies_ms.push_back(
      std::chrono::duration<double, std::milli>(*meta.t_out - *meta.t_cap).count());
}

}  // namespace

std::optional<double> percentile(std::vector<double> values, double p) {
  if (values.empty()) return std::nullopt;
  std::sort(values.begin(), values.end());
  const auto n = static_cast<double>(values.size());
  // ceil() of an exact product such as 0.95 x 100 must not round up to the next rank.
  const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * n - 1e-9));
  return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
}

RunStats compute_run_stats(const std::vector<FrameMeta>& frames,
                           const std::vector<DepthSample>& depth, Clock::time_point t0,
                           Clock::time_point t_end, Clock::duration warmup) {
  RunStats stats;
  const Clock::time_point window_start = t0 + warmup;
  stats.window_s = std::max(0.0, std::chrono::duration<double>(t_end - window_start).count());

  std::vector<double> latencies_ms;
  std::uint64_t delivered_in_window = 0;  // by t_out: a delivery rate, not a capture cohort (D36)
  for (const FrameMeta& meta : frames) {
    count_whole_run(meta, stats);
    if (meta.t_cap && *meta.t_cap >= window_start) count_window(meta, stats, latencies_ms);
    if (meta.t_out && *meta.t_out >= window_start && *meta.t_out < t_end) ++delivered_in_window;
  }

  stats.p50_ms = percentile(latencies_ms, 50);
  stats.p95_ms = percentile(latencies_ms, 95);
  stats.p99_ms = percentile(latencies_ms, 99);
  stats.max_ms = percentile(latencies_ms, 100);

  stats.input_fps = per_second(stats.window_produced, stats.window_s);
  stats.output_fps = per_second(delivered_in_window, stats.window_s);
  const std::uint64_t window_dropped =
      stats.window_dropped_oldest + stats.window_dropped_newest + stats.window_dropped_stale;
  stats.drop_rate_pct = rate_pct(window_dropped, stats.window_produced);
  stats.drop_oldest_pct = rate_pct(stats.window_dropped_oldest, stats.window_produced);
  stats.drop_newest_pct = rate_pct(stats.window_dropped_newest, stats.window_produced);
  stats.drop_stale_pct = rate_pct(stats.window_dropped_stale, stats.window_produced);

  std::size_t depth_sum = 0;
  std::size_t depth_count = 0;
  for (const DepthSample& sample : depth) {
    if (sample.t < window_start) continue;
    depth_sum += sample.depth;
    ++depth_count;
    stats.depth_max = std::max(stats.depth_max.value_or(0), sample.depth);
  }
  if (depth_count > 0) {
    stats.depth_mean = static_cast<double>(depth_sum) / static_cast<double>(depth_count);
  }
  return stats;
}

}  // namespace frameflow
