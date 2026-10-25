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

#include <ostream>

#include "frameflow/cli.hpp"

namespace frameflow {

// euid is the effective user id (a parameter so tests can check the root refusal without root).
int run_command(const RunArgs& args, unsigned euid, std::ostream& out, std::ostream& err);

}  // namespace frameflow
