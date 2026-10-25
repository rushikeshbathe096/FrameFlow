// run_report.hpp
// Responsibility: everything the reporters need about one finished run, built once on the main
// thread: per-frame records with the warm-up flag set (BR-06), depth samples, the event list
// (drops now; adaptive state changes in Phase 4, DR-03), summary statistics and the validity flag
// (BR-09: invalid if stopped before warm-up ended or ended by an error).
// Thread-safety: plain data plus a pure builder; used on the main thread after the pipeline has
// joined.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"
#include "frameflow/metrics/stats.hpp"
#include "frameflow/pipeline.hpp"

namespace frameflow {

struct RunEvent {
  Clock::time_point t;
  std::string type;  // "drop" or "state_change" (adaptive, FR-POL-09)
  std::optional<std::uint64_t> frame_id;
  std::string detail;  // drop reason, or "NORMAL->SHEDDING L_ms=123.456"
};

// Resident memory of the process (PR-07, PR-08, D56), sampled once per second by the main
// thread; empty when the run was too short to sample or /proc was unreadable.
struct MemoryUsage {
  std::optional<double> peak_rss_mb;          // VmHWM at the end
  std::optional<double> rss_after_warmup_mb;  // first sample at or after the end of warm-up
  std::optional<double> rss_end_mb;           // after the run, before results are written
  std::optional<double> growth_pct;           // end vs after warm-up (PR-08)
};

struct RunReport {
  Clock::time_point t0;
  Clock::time_point t_end;
  EndedBy ended_by = EndedBy::Error;
  std::string error;
  bool valid = false;
  std::vector<FrameMeta> frames;  // id order, `warmup` set
  std::vector<DepthSample> depth;
  std::vector<RunEvent> events;  // time order
  RunStats stats;
  MemoryUsage memory;  // filled by the run command after the run (not by make_run_report)
};

RunReport make_run_report(const MetricsStore& metrics, const RunResult& result,
                          Clock::duration warmup);

}  // namespace frameflow
