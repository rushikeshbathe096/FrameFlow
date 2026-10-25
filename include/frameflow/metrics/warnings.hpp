// warnings.hpp
// Responsibility: decides which run-health warnings apply (ER-07, UIUX 9.3): source late by more
// than 5 ms on more than 1% of frames, stopped during warm-up, warm-up longer than half the run,
// and a run shorter than 10 s. Returns plain messages (no prefix); the caller prints them to
// stderr as `frameflow: warning: <message>` and stores them in `summary.json`.
// Thread-safety: pure function; runs on the main thread after the pipeline threads have joined.
#pragma once

#include <string>
#include <vector>

#include "frameflow/config.hpp"
#include "frameflow/metrics/run_report.hpp"

namespace frameflow {

std::vector<std::string> collect_warnings(const RunConfig& config, const RunReport& report);

}  // namespace frameflow
