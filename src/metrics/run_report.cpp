#include "frameflow/metrics/run_report.hpp"

#include <algorithm>
#include <string>

namespace frameflow {

RunReport make_run_report(const MetricsStore& metrics, const RunResult& result,
                          Clock::duration warmup) {
  RunReport report;
  report.t0 = result.t0;
  report.t_end = result.t_end;
  report.ended_by = result.ended_by;
  report.error = result.error;
  const Clock::time_point window_start = result.t0 + warmup;
  report.valid = result.ended_by != EndedBy::Error && result.t_end >= window_start;  // BR-09

  report.frames = metrics.frames();
  for (FrameMeta& meta : report.frames) {
    meta.warmup = meta.t_cap && *meta.t_cap < window_start;
    if (meta.is_dropped() && meta.t_drop) {
      report.events.push_back(
          Event{*meta.t_drop, "drop", meta.id, std::string(drop_reason_name(meta.drop_reason))});
    }
  }
  std::stable_sort(report.events.begin(), report.events.end(),
                   [](const Event& a, const Event& b) { return a.t < b.t; });

  report.depth = metrics.depth_samples();
  report.stats = compute_run_stats(report.frames, report.depth, result.t0, result.t_end, warmup);
  return report;
}

}  // namespace frameflow
