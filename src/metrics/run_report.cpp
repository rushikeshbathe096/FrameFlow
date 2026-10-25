#include "frameflow/metrics/run_report.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string>

namespace frameflow {

namespace {

// "NORMAL->SHEDDING L_ms=123.456" (no commas: it is a CSV field, DR-03).
std::string change_detail(const StateChange& change) {
  char number[48];
  const double l_ms = std::chrono::duration<double, std::milli>(change.latency_estimate).count();
  const std::to_chars_result r =
      std::to_chars(number, number + sizeof(number), l_ms, std::chars_format::fixed, 3);
  return std::string(state_name(change.from)) + "->" + std::string(state_name(change.to)) +
         " L_ms=" + std::string(number, r.ptr);
}

// Share of [window_start, t_end) spent in SHEDDING, in percent (state starts NORMAL).
std::optional<double> shedding_pct(const std::vector<StateChange>& changes,
                                   Clock::time_point window_start, Clock::time_point t_end) {
  if (t_end <= window_start) return std::nullopt;
  Clock::duration shedding{0};
  bool shedding_now = false;
  Clock::time_point since = window_start;
  for (const StateChange& change : changes) {
    const Clock::time_point t = std::clamp(change.t, window_start, t_end);
    if (shedding_now) shedding += t - since;
    since = t;
    shedding_now = change.to == AdaptiveState::Shedding;
  }
  if (shedding_now) shedding += t_end - since;
  return 100.0 * std::chrono::duration<double>(shedding).count() /
         std::chrono::duration<double>(t_end - window_start).count();
}

}  // namespace

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
          RunEvent{*meta.t_drop, "drop", meta.id, std::string(drop_reason_name(meta.drop_reason))});
    }
  }
  if (result.state_changes) {
    for (const StateChange& change : *result.state_changes) {
      report.events.push_back(
          RunEvent{change.t, "state_change", std::nullopt, change_detail(change)});
    }
  }
  std::stable_sort(report.events.begin(), report.events.end(),
                   [](const RunEvent& a, const RunEvent& b) { return a.t < b.t; });

  report.depth = metrics.depth_samples();
  report.stats = compute_run_stats(report.frames, report.depth, result.t0, result.t_end, warmup);
  if (result.state_changes) {  // adaptive only (FR-MET-04, D45)
    report.stats.state_changes = result.state_changes->size();
    report.stats.shedding_pct = shedding_pct(*result.state_changes, window_start, result.t_end);
  }
  return report;
}

}  // namespace frameflow
