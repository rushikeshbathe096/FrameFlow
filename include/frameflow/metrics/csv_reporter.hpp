// csv_reporter.hpp
// Responsibility: writes the per-frame trace `frames.csv` (DR-01), the queue-depth series
// `depth.csv` (DR-02) and the event log `events.csv` (DR-03). UTF-8, comma-separated, `\n` line
// endings, header row (DR-06). Numbers are formatted with std::to_chars, so `.` is the decimal
// separator and no digit grouping appears whatever the locale (DR-07). Only metadata is written,
// never pixels (DR-10). Thread-safety: free functions with no shared state. They run on the main
// thread after the pipeline threads have joined, never on the source or worker thread (FR-MET-07).
#pragma once

#include <filesystem>
#include <ostream>
#include <vector>

#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"
#include "frameflow/metrics/run_report.hpp"

namespace frameflow {

// Times are microseconds since t0 (the run start); a stage the frame never reached is empty.
void write_frames_csv(std::ostream& out, const std::vector<FrameMeta>& frames,
                      Clock::time_point t0);
void write_depth_csv(std::ostream& out, const std::vector<DepthSample>& samples,
                     Clock::time_point t0);
void write_events_csv(std::ostream& out, const std::vector<Event>& events, Clock::time_point t0);

// Writes `frames.csv`, `depth.csv` and `events.csv` into dir (which must exist) with
// write_result_file (O_EXCL, never follows a symlink). Throws std::runtime_error naming the file
// if it cannot be created or written; files written before the failure are kept (ER-05).
void write_csv_files(const std::filesystem::path& dir, const RunReport& report);

}  // namespace frameflow
