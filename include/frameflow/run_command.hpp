// run_command.hpp
// Responsibility: executes `frameflow run`: refuses root (FR-ROLE-02), validates the raw options,
// prepares the output folder safely (output_dir.hpp), prints the effective configuration (S2),
// runs the pipeline with a synthetic source and a null sink (the first SIGINT/SIGTERM stops it
// gracefully, the second exits 130), writes `frames.csv`, `depth.csv`, `events.csv` and
// `summary.json` (FR-MET-05), and prints the summary (S4) and warnings (ER-07). Returns the
// SRS 9.1 exit code.
// Thread-safety: runs on the main thread. All file I/O happens here, before the pipeline threads
// start or after they have joined, never on the source or worker thread (FR-MET-07).
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "frameflow/cli.hpp"
#include "frameflow/metrics/run_report.hpp"
#include "frameflow/platform/environment.hpp"
#include "frameflow/platform/signals.hpp"
#include "frameflow/source/source.hpp"

namespace frameflow {

struct RunOutcome {
  RunReport report;
  std::vector<std::string> warnings;       // ER-07, also in summary.json
  std::optional<std::string> write_error;  // ER-05 error line, when a result file failed
  int exit_code() const;                   // 4 write error, 1 run ended by error, else 0
};

// The configured source: SyntheticSource, or VideoFileSource for `--source=file`, which opens
// and decodes the file now. Throws InputError (exit 3) for a bad file, before any thread starts.
std::unique_ptr<ISource> make_source(const RunConfig& config);

// One run of a validated configuration into dir (which must exist): pipeline with the given
// source and a null sink (or the preview window), the first signal stops it gracefully (via
// signals; `progress`, when given, receives the S3 line once per second), then the four
// result files are written. Used by `run` and by `bench` for every repetition.
RunOutcome run_into(const RunConfig& config, std::unique_ptr<ISource> source,
                    const std::filesystem::path& dir, const Environment& env,
                    SignalWatcher& signals, bool preview = false, std::ostream* progress = nullptr);

// euid is the effective user id (a parameter so tests can check the root refusal without root).
int run_command(const RunArgs& args, unsigned euid, std::ostream& out, std::ostream& err);

}  // namespace frameflow
