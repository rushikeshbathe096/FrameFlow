#include "frameflow/bench/bench_runner.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "frameflow/exit_codes.hpp"
#include "frameflow/platform/privileges.hpp"
#include "frameflow/run_command.hpp"
#include "frameflow/source/video_file_source.hpp"

namespace frameflow {

namespace {

namespace fs = std::filesystem;

std::string fixed1(const std::optional<double>& value) {
  if (!value) return "—";
  char buffer[48];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), *value, std::chars_format::fixed, 1);
  return std::string(buffer, result.ptr);
}

std::string pad(std::string text, std::size_t width) {
  // "—" is 3 bytes but one column wide.
  const std::size_t shown =
      text.size() - 2 * static_cast<std::size_t>(std::count(text.begin(), text.end(), '\xe2'));
  if (shown < width) text.append(width - shown, ' ');
  return text;
}

std::string label(const ExperimentConfig& config) {
  if (config.params.empty()) return "default";
  std::string text;
  for (const auto& [key, value] : config.params) {
    text += (text.empty() ? "" : " ") + key.substr(key.rfind('.') + 1) + "=" + value;
  }
  return text;
}

std::string estimate(double seconds) {
  if (seconds < 120.0) return "~" + std::to_string(static_cast<long>(std::ceil(seconds))) + " s";
  return "~" + std::to_string(static_cast<long>(std::ceil(seconds / 60.0))) + " min";
}

RunStatus status_of(const RunOutcome& outcome, const SignalWatcher& signals) {
  if (outcome.exit_code() != kExitOk) return RunStatus::Failed;
  if (signals.signal_count() > 0 && outcome.report.ended_by == EndedBy::StopRequested) {
    return RunStatus::Interrupted;
  }
  return RunStatus::Completed;
}

std::string outcome_text(const RepResult& r, std::size_t warnings) {
  std::string text = "p95 " + fixed1(r.p95_ms) + " ms   drops " + fixed1(r.drop_pct) + "%   ";
  switch (r.status) {
    case RunStatus::Completed:
      text += r.valid ? "ok" : "invalid";
      break;
    case RunStatus::Interrupted:
      text += "interrupted";
      break;
    case RunStatus::Failed:
    case RunStatus::Missing:
      text += "failed (exit " + std::to_string(r.exit_code) + ")";
      break;
  }
  if (warnings > 0) {
    text += " (" + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings") +
            " in summary.json)";
  }
  return text;
}

RepResult pending(const std::string& config, std::uint64_t rep) {
  RepResult result;
  result.config = config;
  result.rep = rep;
  return result;
}

}  // namespace

std::optional<double> median(std::vector<double> values) {
  if (values.empty()) return std::nullopt;
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2;
  if (values.size() % 2 == 1) return values[mid];
  return (values[mid - 1] + values[mid]) / 2.0;
}

std::string format_median_table(const Experiment& experiment, const std::vector<RepResult>& reps) {
  std::size_t width = 13;
  for (const ExperimentConfig& c : experiment.configs) width = std::max(width, c.name.size());
  std::ostringstream out;
  out << "Median over completed repeats (p95 min-max in brackets)\n"
      << "  " << pad("configuration", width) << "  runs  " << pad("p50 ms", 9) << pad("p95 ms", 22)
      << pad("p99 ms", 9) << pad("drop %", 9) << "out fps\n";
  for (const ExperimentConfig& c : experiment.configs) {
    std::vector<double> p50, p95, p99, drop, fps;
    for (const RepResult& r : reps) {
      if (r.config != c.name || r.status != RunStatus::Completed) continue;
      if (r.p50_ms) p50.push_back(*r.p50_ms);
      if (r.p95_ms) p95.push_back(*r.p95_ms);
      if (r.p99_ms) p99.push_back(*r.p99_ms);
      if (r.drop_pct) drop.push_back(*r.drop_pct);
      if (r.output_fps) fps.push_back(*r.output_fps);
    }
    std::string p95_text = fixed1(median(p95));
    if (!p95.empty()) {
      p95_text += " [" + fixed1(*std::min_element(p95.begin(), p95.end())) + "-" +
                  fixed1(*std::max_element(p95.begin(), p95.end())) + "]";
    }
    const std::size_t runs =
        static_cast<std::size_t>(std::count_if(reps.begin(), reps.end(), [&](const RepResult& r) {
          return r.config == c.name && r.status == RunStatus::Completed;
        }));
    out << "  " << pad(c.name, width) << "  " << pad(std::to_string(runs), 4) << "  "
        << pad(fixed1(median(p50)), 9) << pad(p95_text, 22) << pad(fixed1(median(p99)), 9)
        << pad(fixed1(median(drop)), 9) << fixed1(median(fps)) << '\n';
  }
  return out.str();
}

BenchRunner::BenchRunner(const Experiment& experiment, fs::path root, OutDirOptions options,
                         SignalWatcher& signals, std::ostream& out, std::ostream& err)
    : experiment_(experiment),
      root_(std::move(root)),
      options_(std::move(options)),
      signals_(signals),
      out_(out),
      err_(err),
      env_(collect_environment()) {
  for (const ExperimentConfig& c : experiment_.configs) {
    for (std::uint64_t rep = 1; rep <= experiment_.repeats; ++rep) {
      reps_.push_back(pending(c.name, rep));
    }
  }
}

void BenchRunner::print_header() const {
  double seconds = 0.0;
  for (const ExperimentConfig& c : experiment_.configs) {
    seconds += static_cast<double>(experiment_.repeats) * (c.config.run_seconds() + 0.5);
  }
  out_ << "Experiment " << experiment_.name << "  ·  " << experiment_.configs.size()
       << " configurations x " << experiment_.repeats << " repeats = " << reps_.size()
       << " runs  ·  " << estimate(seconds) << '\n'
       << std::flush;
}

void BenchRunner::write_experiment_json(bool interrupted) const {
  std::vector<RunRecord> records;
  for (const RepResult& r : reps_) records.push_back({r.config, r.rep, r.status, r.exit_code});
  const nlohmann::json json = make_experiment_json(experiment_, records, interrupted);
  const fs::path tmp = root_ / ".experiment.json.tmp";
  std::error_code ec;
  fs::remove(tmp, ec);
  try {
    write_result_file(tmp, [&](std::ostream& out) { out << json.dump(2) << '\n'; });
    fs::rename(tmp, root_ / "experiment.json");  // replaces the old file (or a link) atomically
  } catch (const std::exception& e) {
    err_ << "frameflow: error: cannot update " << (root_ / "experiment.json").string() << ": "
         << e.what() << '\n';
  }
}

RepResult BenchRunner::run_one(const ExperimentConfig& config, std::uint64_t rep,
                               std::size_t index) {
  RepResult result = pending(config.name, rep);
  const std::string total = std::to_string(reps_.size());
  std::string number = std::to_string(index + 1);
  number.insert(0, total.size() - number.size(), ' ');
  out_ << "  [" << number << "/" << total << "]  " << label(config) << " rep=" << rep << "   "
       << std::flush;

  std::error_code ec;
  fs::create_directory(root_ / config.name, ec);  // exists from an earlier rep: fine
  const std::optional<fs::path> dir =
      prepare_out_dir(root_ / config.name / ("rep_" + std::to_string(rep)), options_, err_);
  if (!dir) {
    result.status = RunStatus::Failed;
    result.exit_code = kExitOutput;
    out_ << "failed (exit 4)\n";
    return result;
  }
  std::unique_ptr<ISource> source;
  try {
    source = make_source(config.config);
  } catch (const InputError& e) {
    err_ << "frameflow: error: " << e.what() << " (hint: " << e.hint() << ")\n";
    result.status = RunStatus::Failed;
    result.exit_code = kExitInput;
    out_ << "failed (exit 3)\n";
    return result;
  }
  const RunOutcome outcome = run_into(config.config, std::move(source), *dir, env_, signals_);
  const RunStats& s = outcome.report.stats;
  result.status = status_of(outcome, signals_);
  result.exit_code = outcome.exit_code();
  result.p50_ms = s.p50_ms;
  result.p95_ms = s.p95_ms;
  result.p99_ms = s.p99_ms;
  result.drop_pct = s.drop_rate_pct;
  result.output_fps = s.output_fps;
  result.valid = outcome.report.valid;
  out_ << outcome_text(result, outcome.warnings.size()) << '\n' << std::flush;
  if (outcome.write_error) err_ << *outcome.write_error;
  if (outcome.report.ended_by == EndedBy::Error) {
    err_ << "frameflow: error: " << outcome.report.error << " (in " << dir->string() << ")\n";
  }
  return result;
}

int BenchRunner::run() {
  print_header();
  write_experiment_json(false);
  bool interrupted = false;
  for (std::size_t i = 0; i < reps_.size() && !interrupted; ++i) {
    if (signals_.signal_count() > 0) {  // stopped between runs
      interrupted = true;
      break;
    }
    const auto config =
        std::find_if(experiment_.configs.begin(), experiment_.configs.end(),
                     [&](const ExperimentConfig& c) { return c.name == reps_[i].config; });
    reps_[i] = run_one(*config, reps_[i].rep, i);
    interrupted = reps_[i].status == RunStatus::Interrupted;
    write_experiment_json(interrupted);
  }
  write_experiment_json(interrupted);
  out_ << '\n' << format_median_table(experiment_, reps_);
  if (interrupted) {
    std::size_t missing = 0;
    for (const RepResult& r : reps_) {
      if (r.status == RunStatus::Missing || r.status == RunStatus::Interrupted) ++missing;
    }
    err_ << "frameflow: info: bench interrupted; " << missing << " of " << reps_.size()
         << " runs missing (listed in " << (root_ / "experiment.json").string() << "):\n";
    for (const RepResult& r : reps_) {
      if (r.status == RunStatus::Missing || r.status == RunStatus::Interrupted) {
        err_ << "  " << r.config << "/rep_" << r.rep << '\n';
      }
    }
  }
  out_ << "\n  Experiment  " << (root_ / "experiment.json").string() << '\n';
  const bool failed = std::any_of(reps_.begin(), reps_.end(),
                                  [](const RepResult& r) { return r.status == RunStatus::Failed; });
  return failed ? kExitRuntime : kExitOk;
}

int bench_command(const BenchArgs& args, unsigned euid, std::ostream& out, std::ostream& err) {
  if (const auto refused = check_root(euid, args.allow_root, err)) return *refused;
  SignalWatcher signals;  // before any pipeline thread starts (see run_command)
  const ExperimentLoad load = load_experiment(args.config, args.repeats);
  if (!load.ok()) {
    for (const std::string& e : load.errors) err << "frameflow: error: " << e << '\n';
    err << "Run 'frameflow bench --help' to see all options.\n";
    return kExitUsage;
  }
  std::error_code ec;
  if (!args.out) fs::create_directories("results", ec);  // the default parent (D42)
  const fs::path parent = args.out ? fs::path(*args.out) : fs::path("results");
  OutDirOptions options{args.overwrite, args.allow_any_out, default_allowed_roots()};
  const std::optional<fs::path> root = prepare_out_dir(parent / load.experiment.name, options, err);
  if (!root) return kExitOutput;
  return BenchRunner(load.experiment, *root, options, signals, out, err).run();
}

}  // namespace frameflow
