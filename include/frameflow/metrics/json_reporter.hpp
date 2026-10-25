// json_reporter.hpp
// Responsibility: builds and writes `summary.json` (DR-04): the effective configuration and seed,
// the environment, the run metrics, validity, warnings, FrameFlow version, git commit and schema
// version, so a run can be reproduced (BR-10). A metric with no value (e.g. latency when nothing
// was delivered) is written as `null`, never 0 (EC-15). Metric keys reuse the RunStats field
// names (D24). nlohmann-json formats numbers without the locale, so `.` is always the decimal
// separator (DR-07).
// Thread-safety: free functions with no shared state. They run on the main thread after the
// pipeline threads have joined, never on the source or worker thread (FR-MET-07).
#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "frameflow/config.hpp"
#include "frameflow/metrics/run_report.hpp"
#include "frameflow/platform/environment.hpp"

namespace frameflow {

inline constexpr int kSummarySchemaVersion = 1;

nlohmann::json make_summary_json(const RunConfig& config, const RunReport& report,
                                 const Environment& env, const std::vector<std::string>& warnings);

// Writes `summary.json` into dir (which must exist) with write_result_file (O_EXCL), indented by
// 2 spaces with a final newline. Throws std::runtime_error naming the file on failure.
void write_summary_json(const std::filesystem::path& dir, const nlohmann::json& summary);

}  // namespace frameflow
