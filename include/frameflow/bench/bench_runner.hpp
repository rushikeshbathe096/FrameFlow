// bench_runner.hpp
// Responsibility: `frameflow bench` (FR-CLI-02, DR-05, UIUX S5). Runs every configuration of an
// experiment `repeats` times, one after another, each into `<out>/<experiment>/<config>/rep_<n>/`
// with the same four result files as `run` (D42). Prints one progress line per run and, at the
// end, the median table across repeats with min-max of p95 (BR-11). `experiment.json` is
// rewritten after every run (temp file + rename), so it always lists completed, failed and
// missing runs. The first SIGINT/SIGTERM stops the current run gracefully and skips the rest
// (EC-20); the second exits 130 (signals.hpp).
// Thread-safety: BenchRunner runs on the main thread; each run's pipeline threads are joined
// before the next starts.
#pragma once

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "frameflow/bench/experiment.hpp"
#include "frameflow/cli.hpp"
#include "frameflow/platform/environment.hpp"
#include "frameflow/platform/output_dir.hpp"
#include "frameflow/platform/signals.hpp"

namespace frameflow {

// One finished repetition, as shown in progress and used for the median table.
struct RepResult {
  std::string config;
  std::uint64_t rep = 1;
  RunStatus status = RunStatus::Missing;
  int exit_code = 0;
  std::optional<double> p50_ms, p95_ms, p99_ms, drop_pct, output_fps;
  bool valid = false;
};

// Median of the values (mean of the two middle ones for an even count); empty -> nullopt.
std::optional<double> median(std::vector<double> values);

// BR-11: one row per configuration with medians over its completed repetitions.
std::string format_median_table(const Experiment& experiment, const std::vector<RepResult>& reps);

class BenchRunner {
 public:
  BenchRunner(const Experiment& experiment, std::filesystem::path root, OutDirOptions options,
              SignalWatcher& signals, std::ostream& out, std::ostream& err);

  // Runs everything (or until a signal). Returns 0, or 1 if any run failed.
  int run();

 private:
  RepResult run_one(const ExperimentConfig& config, std::uint64_t rep, std::size_t index);
  void write_experiment_json(bool interrupted) const;
  void print_header() const;

  const Experiment& experiment_;
  std::filesystem::path root_;
  OutDirOptions options_;
  SignalWatcher& signals_;
  std::ostream& out_;
  std::ostream& err_;
  Environment env_;
  std::vector<RepResult> reps_;  // one per (config, rep) in run order, Missing until run
};

int bench_command(const BenchArgs& args, unsigned euid, std::ostream& out, std::ostream& err);

}  // namespace frameflow
