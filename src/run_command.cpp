#include "frameflow/run_command.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "frameflow/clock.hpp"
#include "frameflow/metrics/csv_reporter.hpp"
#include "frameflow/metrics/json_reporter.hpp"
#include "frameflow/metrics/run_report.hpp"
#include "frameflow/metrics/warnings.hpp"
#include "frameflow/pipeline.hpp"
#include "frameflow/platform/environment.hpp"
#include "frameflow/platform/output_dir.hpp"
#include "frameflow/platform/privileges.hpp"
#include "frameflow/platform/signals.hpp"
#include "frameflow/sink/null_sink.hpp"
#include "frameflow/source/synthetic_source.hpp"
#include "frameflow/ui/console_reporter.hpp"

namespace frameflow {

namespace {

namespace fs = std::filesystem;

// The folder the user asked for, or the next free `results/run_NNN` (D26).
fs::path requested_out(const RunArgs& args) {
  if (args.out) return *args.out;
  std::error_code ec;
  fs::create_directories("results", ec);  // the default's parent; a failure is reported later
  return next_run_dir("results");
}

// Routes the first SIGINT/SIGTERM to pipeline.request_stop() while the pipeline exists; the
// destructor detaches it before the pipeline is destroyed, also when run() throws.
class StopOnSignal {
 public:
  StopOnSignal(SignalWatcher& signals, Pipeline& pipeline) : signals_(signals) {
    signals_.set_stop_handler([&pipeline] { pipeline.request_stop(); });
  }
  ~StopOnSignal() { signals_.clear_stop_handler(); }
  StopOnSignal(const StopOnSignal&) = delete;
  StopOnSignal& operator=(const StopOnSignal&) = delete;

 private:
  SignalWatcher& signals_;
};

RunReport execute(const RunConfig& config, SignalWatcher& signals) {
  SteadyClock clock;  // outlives the pipeline: the sink holds a reference to it
  Pipeline pipeline(config.to_pipeline_config(), clock,
                    std::make_unique<SyntheticSource>(config.resolution),
                    std::make_unique<NullSink>(clock));
  RunResult result;
  {
    const StopOnSignal stop_on_signal(signals, pipeline);
    result = pipeline.run();
  }
  const auto warmup =
      std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(config.warmup_s));
  return make_run_report(pipeline.metrics(), result, warmup);
}

// ER-05: returns the error line naming the failing file; files already written are kept.
std::optional<std::string> write_results(const fs::path& dir, const RunConfig& config,
                                         const RunReport& report, const Environment& env,
                                         const std::vector<std::string>& warnings) {
  try {
    write_csv_files(dir, report);
    write_summary_json(dir, make_summary_json(config, report, env, warnings));
  } catch (const std::runtime_error& e) {
    return "frameflow: error: " + std::string(e.what()) + "; files already written are kept in " +
           dir.string() + " (hint: check free disk space and permissions)\n";
  }
  return std::nullopt;
}

}  // namespace

int run_command(const RunArgs& args, unsigned euid, std::ostream& out, std::ostream& err) {
  if (const auto refused = check_root(euid, args.allow_root, err)) return *refused;
  // First, before any pipeline thread starts, so they all inherit the blocked signals and a
  // signal during setup is not lost (it stops the run as soon as it starts). Alive until the
  // files are written, so a second signal while writing still exits 130 at once.
  SignalWatcher signals;
  const ValidationResult validated = validate(args.raw);
  if (!validated.ok()) {
    err << format_validation_errors(validated.errors);
    return kExitUsage;
  }
  const RunConfig& config = validated.config;
  const fs::path shown = requested_out(args);
  const OutDirOptions options{args.overwrite, args.allow_any_out, default_allowed_roots()};
  const std::optional<fs::path> dir = prepare_out_dir(shown, options, err);
  if (!dir) return kExitOutput;

  out << format_run_start(config, shown) << std::flush;
  const Environment env = collect_environment();  // file reads: before the threads start
  const RunReport report = execute(config, signals);
  const std::vector<std::string> warnings = collect_warnings(config, report);
  const std::optional<std::string> write_error = write_results(*dir, config, report, env, warnings);

  out << '\n' << format_run_summary(config, report) << std::flush;  // S4, then its warnings
  err << format_warnings(warnings) << std::flush;
  if (write_error) {
    err << *write_error;
    return kExitOutput;
  }
  if (report.ended_by == EndedBy::Error) {
    err << "frameflow: error: " << report.error << "; partial results saved to " << shown.string()
        << " (marked invalid)\n";
    return kExitRuntime;
  }
  out << "\n  Files  " << shown.string() << "/{frames.csv, depth.csv, events.csv, summary.json}\n";
  return kExitOk;
}

}  // namespace frameflow
