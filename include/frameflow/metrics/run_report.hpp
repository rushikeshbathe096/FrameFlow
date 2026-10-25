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

struct Event {
  Clock::time_point t;
  std::string type;  // "drop"
  std::optional<std::uint64_t> frame_id;
  std::string detail;  // drop reason
};

struct RunReport {
  Clock::time_point t0;
  Clock::time_point t_end;
  EndedBy ended_by = EndedBy::Error;
  std::string error;
  bool valid = false;
  std::vector<FrameMeta> frames;  // id order, `warmup` set
  std::vector<DepthSample> depth;
  std::vector<Event> events;  // time order
  RunStats stats;
};

RunReport make_run_report(const MetricsStore& metrics, const RunResult& result,
                          Clock::duration warmup);

}  // namespace frameflow
