// console_reporter.hpp
// Responsibility: the end-of-run console summary (UIUX S4, FR-MET-06): latency percentiles,
// input/output FPS, frame counts with drop reasons, queue depth, and validity, plus the warning
// lines (ER-07). Frame counts cover the whole run so conservation is visible (BR-01); latency,
// FPS, drop rate and depth are after warm-up (D19), and a note says so when warm-up > 0. Empty
// states follow UIUX 9.4 ("no frames delivered", no reason breakdown without drops).
// Thread-safety: pure functions returning strings; run on the main thread after the pipeline
// threads have joined.
#pragma once

#include <string>
#include <vector>

#include "frameflow/config.hpp"
#include "frameflow/metrics/run_report.hpp"

namespace frameflow {

std::string format_run_summary(const RunConfig& config, const RunReport& report);

// One `frameflow: warning: <message>` line per warning (ER-07, ER-01 style prefix).
std::string format_warnings(const std::vector<std::string>& warnings);

}  // namespace frameflow
