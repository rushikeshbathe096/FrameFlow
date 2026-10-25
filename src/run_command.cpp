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
#include "frameflow/config_file.hpp"
#include "frameflow/metrics/csv_reporter.hpp"
#include "frameflow/metrics/json_reporter.hpp"
#include "frameflow/metrics/run_report.hpp"
#include "frameflow/metrics/warnings.hpp"
#include "frameflow/pipeline.hpp"
#include "frameflow/platform/display.hpp"
#include "frameflow/platform/environment.hpp"
#include "frameflow/platform/output_dir.hpp"
#include "frameflow/platform/privileges.hpp"
#include "frameflow/platform/signals.hpp"
#include "frameflow/sink/null_sink.hpp"
#include "frameflow/sink/preview_sink.hpp"
#include "frameflow/source/synthetic_source.hpp"
#include "frameflow/source/video_file_source.hpp"
#include "frameflow/ui/console_reporter.hpp"
#include "frameflow/ui/progress.hpp"

namespace frameflow {

namespace {

namespace fs = std::filesystem;

// The flags merged over the --config file (flags win, EC-19); std::nullopt after printing the
// file's errors (VR-18, exit 2).
std::optional<RawConfig> effective_raw(const RunArgs& args, std::ostream& err) {
  if (!args.config) return args.raw;
  const ConfigFileResult file = load_run_config(*args.config);
  if (!file.ok()) {
    for (const std::string& e : file.errors) err << "frameflow: error: " << e << '\n';
    err << "Run 'frameflow run --help' to see all options.\n";
    return std::nullopt;
  }
  return merge_raw(file.raw, args.raw);
}

// --preview: on, auto or absent. Sets `enabled` and returns 0, or prints the error and returns
// the exit code: 2 for an unknown value, 6 for "on" without a display (ER-06). "auto" without a
// display warns and runs without preview.
int check_preview(const RunArgs& args, std::ostream& err, bool& enabled) {
  enabled = false;
  if (!args.preview) return kExitOk;
  if (*args.preview != "on" && *args.preview != "auto") {
    err << "frameflow: error: 1 invalid option\n  --preview " << *args.preview
        << " must be on or auto (hint: use --preview or --preview=auto)\n";
    return kExitUsage;
  }
  if (display_available()) {
    enabled = true;
    return kExitOk;
  }
  if (*args.preview == "on") {
    err << "frameflow: error: --preview needs a display (hint: use --preview=auto or remove "
           "--preview)\n";
    return kExitDisplay;
  }
  err << "frameflow: warning: no display (DISPLAY and WAYLAND_DISPLAY are unset); running "
         "without preview\n";
  return kExitOk;
}

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

// The preview sink reads the shedding state through `pipeline`, which is set before run() starts
// the worker thread (thread start orders the write before every call).
std::unique_ptr<ISink> make_sink(Clock& clock, const RunConfig& config, PreviewMailbox* mailbox,
                                 Pipeline* const& pipeline) {
  if (mailbox == nullptr) return std::make_unique<NullSink>(clock);
  return std::make_unique<PreviewSink>(clock, *mailbox, config.policy, [&pipeline] {
    return pipeline != nullptr && pipeline->shedding();
  });
}

// Work on the thread running Pipeline::run (main), so pipeline threads do no I/O and no GUI:
// every tick, the preview window (D57); once per second, a memory sample (D56) and, with
// --progress, the status line from atomics (D52).
struct Tick {
  Pipeline& pipeline;
  std::ostream* progress;
  ProgressLine& line;
  Clock::duration warmup;
  PreviewDisplay* display;
  std::optional<std::uint64_t> rss_after_warmup_kb;
  std::optional<Clock::duration> last_second;

  void operator()() {
    if (display != nullptr) display->pump();
    const Pipeline::LiveStatus status = pipeline.live_status();
    if (last_second && status.elapsed - *last_second < Pipeline::kTickInterval) return;
    last_second = status.elapsed;
    if (!rss_after_warmup_kb && status.elapsed >= warmup) {
      rss_after_warmup_kb = read_memory_status().rss_kb;
    }
    if (progress != nullptr) *progress << line.update(status) << std::flush;
  }
};

double to_mb(std::uint64_t kb) { return static_cast<double>(kb) / 1024.0; }

MemoryUsage memory_usage(const std::optional<std::uint64_t>& after_warmup_kb) {
  const MemoryStatus end = read_memory_status();
  MemoryUsage usage;
  if (end.peak_kb) usage.peak_rss_mb = to_mb(*end.peak_kb);
  if (end.rss_kb) usage.rss_end_mb = to_mb(*end.rss_kb);
  if (after_warmup_kb) usage.rss_after_warmup_mb = to_mb(*after_warmup_kb);
  if (end.rss_kb && after_warmup_kb && *after_warmup_kb > 0) {
    usage.growth_pct = 100.0 *
                       (static_cast<double>(*end.rss_kb) - static_cast<double>(*after_warmup_kb)) /
                       static_cast<double>(*after_warmup_kb);
  }
  return usage;
}

RunReport execute(const RunConfig& config, std::unique_ptr<ISource> source, bool preview,
                  std::ostream* progress, SignalWatcher& signals) {
  SteadyClock clock;                 // outlives the pipeline: the sink holds a reference to it
  Pipeline* pipeline_ptr = nullptr;  // outlives the pipeline: the preview sink reads it
  PreviewMailbox mailbox;            // outlives the pipeline: the preview sink posts into it
  // The window lives on this (main) thread: created on its first show, destroyed on return.
  std::optional<HighGuiWindow> window;
  if (preview) window.emplace("FrameFlow \u2014 " + config.policy);
  Pipeline pipeline(config.to_pipeline_config(), clock, std::move(source),
                    make_sink(clock, config, preview ? &mailbox : nullptr, pipeline_ptr));
  pipeline_ptr = &pipeline;
  std::optional<PreviewDisplay> display;
  if (window) display.emplace(*window, mailbox, [&pipeline] { pipeline.request_stop(); });
  const auto to_clock = [](double s) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
  };
  ProgressLine line({to_clock(config.run_seconds()), to_clock(config.warmup_s), config.queue},
                    stderr_is_terminal());
  Tick tick{
      pipeline,     progress,    line, to_clock(config.warmup_s), display ? &*display : nullptr,
      std::nullopt, std::nullopt};
  pipeline.set_tick([&tick] { tick(); },
                    display ? Clock::duration(PreviewDisplay::kInterval) : Pipeline::kTickInterval);
  RunResult result;
  {
    const StopOnSignal stop_on_signal(signals, pipeline);
    result = pipeline.run();
  }
  if (progress != nullptr) *progress << line.finish() << std::flush;
  RunReport report = make_run_report(pipeline.metrics(), result, to_clock(config.warmup_s));
  report.memory = memory_usage(tick.rss_after_warmup_kb);
  return report;
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

int RunOutcome::exit_code() const {
  if (write_error) return kExitOutput;
  if (report.ended_by == EndedBy::Error) return kExitRuntime;
  return kExitOk;
}

std::unique_ptr<ISource> make_source(const RunConfig& config) {
  if (config.source == "file" && config.input) {
    return std::make_unique<VideoFileSource>(*config.input, config.loop);
  }
  return std::make_unique<SyntheticSource>(config.resolution);
}

RunOutcome run_into(const RunConfig& config, std::unique_ptr<ISource> source, const fs::path& dir,
                    const Environment& env, SignalWatcher& signals, bool preview,
                    std::ostream* progress) {
  RunOutcome outcome;
  outcome.report = execute(config, std::move(source), preview, progress, signals);
  outcome.warnings = collect_warnings(config, outcome.report);
  outcome.write_error = write_results(dir, config, outcome.report, env, outcome.warnings);
  return outcome;
}

int run_command(const RunArgs& args, unsigned euid, std::ostream& out, std::ostream& err) {
  if (const auto refused = check_root(euid, args.allow_root, err)) return *refused;
  // First, before any pipeline thread starts, so they all inherit the blocked signals and a
  // signal during setup is not lost (it stops the run as soon as it starts). Alive until the
  // files are written, so a second signal while writing still exits 130 at once.
  SignalWatcher signals;
  const std::optional<RawConfig> raw = effective_raw(args, err);
  if (!raw) return kExitUsage;
  const ValidationResult validated = validate(*raw);
  if (!validated.ok()) {
    err << format_validation_errors(validated.errors);
    return kExitUsage;
  }
  const RunConfig& config = validated.config;
  bool preview = false;
  if (const int code = check_preview(args, err, preview); code != kExitOk) return code;  // ER-06
  std::unique_ptr<ISource> source;
  try {
    source = make_source(config);  // VR-14: a bad input file stops here, before any thread
  } catch (const InputError& e) {
    err << "frameflow: error: " << e.what() << " (hint: " << e.hint() << ")\n";
    return kExitInput;
  }
  const fs::path shown = requested_out(args);
  const OutDirOptions options{args.overwrite, args.allow_any_out, default_allowed_roots()};
  const std::optional<fs::path> dir = prepare_out_dir(shown, options, err);
  if (!dir) return kExitOutput;

  const auto* video = dynamic_cast<const VideoFileSource*>(source.get());
  out << format_run_start(config, shown,
                          video ? std::optional<cv::Size>(video->frame_size()) : std::nullopt)
      << std::flush;
  const Environment env = collect_environment();  // file reads: before the threads start
  const RunOutcome outcome = run_into(config, std::move(source), *dir, env, signals, preview,
                                      args.progress ? &err : nullptr);

  out << '\n' << format_run_summary(config, outcome.report) << std::flush;  // S4, then warnings
  err << format_warnings(outcome.warnings) << std::flush;
  if (outcome.write_error) {
    err << *outcome.write_error;
    return kExitOutput;
  }
  if (outcome.report.ended_by == EndedBy::Error) {
    err << "frameflow: error: " << outcome.report.error << "; partial results saved to "
        << shown.string() << " (marked invalid)\n";
    return kExitRuntime;
  }
  out << "\n  Files  " << shown.string() << "/{frames.csv, depth.csv, events.csv, summary.json}\n";
  return kExitOk;
}

}  // namespace frameflow
