#include "frameflow/cli.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <charconv>
#include <exception>
#include <sstream>

#include "frameflow/bench/bench_runner.hpp"
#include "frameflow/platform/privileges.hpp"
#include "frameflow/run_command.hpp"
#include "frameflow/ui/console_reporter.hpp"
#include "frameflow/version.hpp"

namespace frameflow {

namespace {

// Shortest round-trip form without the locale, e.g. 30, 29.97 (DR-07 style).
std::string number_text(double value) {
  char buffer[32];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

// "blur (kernel 15) + " for a real processor, "" for none.
std::string processor_text(const RunConfig& config) {
  if (config.processor == "none") return "";
  if (config.processor == "blur") return "blur (kernel " + std::to_string(config.kernel) + ") + ";
  return config.processor + " + ";
}

// ", spikes 80 ms for 1 s every 5 s" in bursty mode, else "".
std::string spike_text(const RunConfig& config) {
  if (!config.spike_ms || !config.spike_len_s || !config.spike_every_s) return "";
  return ", spikes " + number_text(*config.spike_ms) + " ms for " +
         number_text(*config.spike_len_s) + " s every " + number_text(*config.spike_every_s) + " s";
}

// " (target 100 ms, exit 70 ms, window 30)" for adaptive (UIUX S2), else "".
std::string adaptive_text(const RunConfig& config) {
  if (!config.target_ms || !config.exit_ratio || !config.window) return "";
  return " (target " + number_text(*config.target_ms) + " ms, exit " +
         number_text(*config.target_ms * *config.exit_ratio) + " ms, window " +
         std::to_string(*config.window) + ")";
}

std::size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diagonal = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t above = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  return row[b.size()];
}

std::string version_text() {
  return std::string("frameflow ") + version() + " (git " + git_commit() + ")";
}

struct FlagSpec {
  const char* flag;
  const char* value;  // shown after the flag, e.g. "<1-240>"
  const char* help;
  const char* default_text;
  const char* group;
};

// Flags are grouped in pipeline order (UIUX 3); defaults match SRS section 7.
void add_run_options(CLI::App& run, RunArgs& args) {
  RawConfig& r = args.raw;
  const std::vector<std::pair<FlagSpec, std::optional<std::string>*>> flags = {
      {{"--source", "<file|synthetic>", "frame source", "file", "Source"}, &r.source},
      {{"--input", "<path>", "video file (needs --source=file)", "none", "Source"}, &r.input},
      {{"--fps", "<1-240>", "frames per second", "30", "Source"}, &r.fps},
      {{"--resolution", "<WxH>", "synthetic frame size", "1280x720", "Source"}, &r.resolution},
      {{"--processor", "<name>", "none, grayscale, blur or canny", "none", "Processing"},
       &r.processor},
      {{"--kernel", "<3-99>", "blur kernel size (odd)", "15", "Processing"}, &r.kernel},
      {{"--cost-mode", "<fixed|bursty>", "processing cost model", "fixed", "Processing"},
       &r.cost_mode},
      {{"--cost-ms", "<0-10000>", "processing cost per frame (ms)", "0", "Processing"}, &r.cost_ms},
      {{"--cost-wait", "<busy|sleep>", "how the cost is spent", "busy", "Processing"},
       &r.cost_wait},
      {{"--spike-ms", "<0-10000>", "cost per frame during a spike (bursty)", "none", "Processing"},
       &r.spike_ms},
      {{"--spike-len", "<duration>", "spike length, e.g. 1s or 500ms (bursty)", "none",
        "Processing"},
       &r.spike_len},
      {{"--spike-every", "<duration>", "spike period, e.g. 5s (bursty)", "none", "Processing"},
       &r.spike_every},
      {{"--queue", "<1-1024>", "queue capacity (frames)", "8", "Queue and policy"}, &r.queue},
      {{"--policy", "<name>", "blocking, drop-oldest, drop-newest or adaptive", "blocking",
        "Queue and policy"},
       &r.policy},
      {{"--target-ms", "<1-60000>", "adaptive: latency target T (ms)", "100", "Queue and policy"},
       &r.target_ms},
      {{"--exit-ratio", "<0-1>", "adaptive: leave shedding below exit-ratio x T", "0.7",
        "Queue and policy"},
       &r.exit_ratio},
      {{"--window", "<5-1000>", "adaptive: delivered frames in the p95 estimate", "30",
        "Queue and policy"},
       &r.window},
      {{"--duration", "<1-3600>", "run length (s)", "60", "Run control"}, &r.duration},
      {{"--frames", "<1-1000000>", "run length in frames instead of --duration", "none",
        "Run control"},
       &r.frames},
      {{"--warmup", "<seconds>", "start of the run excluded from statistics", "3", "Run control"},
       &r.warmup},
      {{"--seed", "<uint64>", "random seed", "42", "Run control"}, &r.seed},
      {{"--config", "<file>", "YAML settings; flags override it", "none", "Run control"},
       &args.config},
      {{"--out", "<dir>", "output folder (must not exist)", "results/run_NNN", "Output"},
       &args.out},
  };
  for (const auto& [spec, target] : flags) {
    run.add_option(spec.flag, *target, spec.help)
        ->type_name(spec.value)
        ->default_str(spec.default_text)
        ->group(spec.group);
  }
  run.add_flag("--no-loop", args.no_loop, "stop at the end of the video instead of looping")
      ->group("Source");
  run.add_flag("--preview{on}", args.preview,
               "show the processed video; --preview=auto runs without it when there is no "
               "display")
      ->group("Output");
  run.add_flag("--progress", args.progress, "print a status line every second (stderr)")
      ->group("Output");
  run.add_flag("--overwrite", args.overwrite, "reuse an existing --out folder")->group("Output");
  run.add_flag("--allow-any-out", args.allow_any_out,
               "allow --out to resolve outside your home and the current folder")
      ->group("Output");
  run.add_flag("--allow-root", args.allow_root, "run even as root (not recommended)")
      ->group("Output");
  run.allow_extras();  // unknown flags are reported by report_unknown_flags with a suggestion
}

std::vector<std::string> flag_names(const CLI::App& app) {
  std::vector<std::string> names;
  for (const CLI::Option* option : app.get_options()) {
    for (const std::string& name : option->get_lnames()) names.push_back("--" + name);
  }
  return names;
}

// VR-21: one ER-01 line per unknown argument. Returns false if there were any.
bool report_unknown_flags(const CLI::App& run, std::ostream& err) {
  const std::vector<std::string> extras = run.remaining();
  const std::vector<std::string> known = flag_names(run);
  for (const std::string& extra : extras) {
    const std::string flag = extra.substr(0, extra.find('='));
    if (flag.rfind("-", 0) == 0) {
      err << "frameflow: error: unknown option: " << flag << " (hint: did you mean "
          << closest_flag(flag, known) << "?)\n";
    } else {
      err << "frameflow: error: unexpected argument: " << extra
          << " (hint: options take the form --name=value)\n";
    }
  }
  return extras.empty();
}

// FR-CLI-02: `frameflow bench --config <file>`; run settings come from the file only (D42).
void add_bench_options(CLI::App& bench, BenchArgs& args) {
  bench.add_option("--config", args.config, "experiment YAML file (see experiments/template.yaml)")
      ->type_name("<file>")
      ->required()
      ->group("Experiment");
  bench.add_option("--repeats", args.repeats, "runs per configuration, 1-100 (beats the file)")
      ->type_name("<1-100>")
      ->default_str("5")
      ->group("Experiment");
  bench.add_option("--out", args.out, "parent folder of <experiment>/")
      ->type_name("<dir>")
      ->default_str("results")
      ->group("Output");
  bench.add_flag("--overwrite", args.overwrite, "reuse an existing experiment folder")
      ->group("Output");
  bench
      .add_flag("--allow-any-out", args.allow_any_out,
                "allow --out to resolve outside your home and the current folder")
      ->group("Output");
  bench.add_flag("--allow-root", args.allow_root, "run even as root (not recommended)")
      ->group("Output");
  bench.allow_extras();
  bench.footer("Defaults are shown in [brackets]. Ctrl+C keeps finished runs and lists the rest.");
}

void configure_app(CLI::App& app, CLI::App& run) {
  app.name("frameflow");
  app.get_formatter()->column_width(40);
  app.description(std::string("FrameFlow ") + version() + " - latency-aware video pipeline");
  app.set_version_flag("--version", version_text(), "print version and git commit");
  app.require_subcommand(0, 1);  // a missing command is reported by parse_cli (clearer message)
  app.footer(
      "Examples:\n"
      "  frameflow run --source=synthetic --fps=30 --cost-ms=50 --policy=drop-oldest\n"
      "  frameflow run --source=synthetic --frames=300 --out=results/quick\n"
      "  frameflow bench --config=experiments/e2_sustained.yaml\n\n"
      "Run 'frameflow run --help' or 'frameflow bench --help' for all options.");
  run.footer("Defaults are shown in [brackets]. Values use --name=value or --name value.");
}

}  // namespace

std::string closest_flag(std::string_view unknown, const std::vector<std::string>& known) {
  std::string best;
  std::size_t best_distance = static_cast<std::size_t>(-1);
  for (const std::string& name : known) {
    const std::size_t distance = edit_distance(unknown, name);
    if (distance < best_distance) {
      best_distance = distance;
      best = name;
    }
  }
  return best;
}

CliParse parse_cli(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
  CLI::App app;
  RunArgs args;
  BenchArgs bench_args;
  CLI::App* run = app.add_subcommand("run", "one pipeline run");
  CLI::App* bench = app.add_subcommand("bench", "experiment matrix from a YAML file");
  configure_app(app, *run);
  add_run_options(*run, args);
  add_bench_options(*bench, bench_args);

  CliParse result;
  try {
    app.parse(argc, argv);
  } catch (const CLI::CallForHelp& e) {
    result.exit_code = app.exit(e, out, err);
    return result;
  } catch (const CLI::CallForVersion& e) {
    result.exit_code = app.exit(e, out, err);
    return result;
  } catch (const CLI::ParseError& e) {
    err << "frameflow: error: " << e.what() << " (hint: run 'frameflow --help')\n";
    result.exit_code = kExitUsage;
    return result;
  }
  if (!run->parsed() && !bench->parsed()) {
    err << "frameflow: error: no command given (hint: try 'frameflow run --help')\n";
    result.exit_code = kExitUsage;
    return result;
  }
  if (!report_unknown_flags(run->parsed() ? *run : *bench, err)) {
    result.exit_code = kExitUsage;
    return result;
  }
  if (run->parsed()) {
    if (args.no_loop) args.raw.loop = "false";  // a flag, so it beats a config file (EC-19)
    result.run = std::move(args);
  } else {
    result.bench = std::move(bench_args);
  }
  return result;
}

std::string format_run_start(const RunConfig& config, const std::filesystem::path& out_dir,
                             std::optional<cv::Size> source_size) {
  const cv::Size size = source_size.value_or(config.resolution);
  const std::string source = config.source == "file" && config.input
                                 ? "file " + *config.input + (config.loop ? "" : " (no loop)")
                                 : config.source;
  std::ostringstream text;
  const std::string length = config.frames ? std::to_string(*config.frames) + " frames"
                                           : number_text(config.duration_s.value_or(0.0)) + " s";
  text << "FrameFlow " << version() << "  ·  run started\n"
       << "  source     " << source << ' ' << size.width << 'x' << size.height << " @ "
       << number_text(config.fps) << " fps\n"
       << "  processing " << processor_text(config) << "cost " << number_text(config.cost_ms)
       << " ms" << spike_text(config) << " (" << config.cost_mode << ", "
       << (config.cost_wait == CostWait::Busy ? "busy" : "sleep") << ")  ·  "
       << overload_text(config) << "\n"
       << "  queue      " << config.queue << " frames  ·  policy " << config.policy
       << adaptive_text(config) << '\n'
       << "  run        " << length << " (warm-up " << number_text(config.warmup_s)
       << " s)  ·  seed " << config.seed << '\n'
       << "  output     " << out_dir.string() << "/\n";
  return text.str();
}

int cli_main(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
  try {
    const CliParse parsed = parse_cli(argc, argv, out, err);
    if (parsed.run) return run_command(*parsed.run, effective_uid(), out, err);
    if (parsed.bench) return bench_command(*parsed.bench, effective_uid(), out, err);
    return parsed.exit_code;
  } catch (const std::exception& e) {
    err << "frameflow: error: internal error: " << e.what() << '\n';
    return kExitRuntime;
  }
}

}  // namespace frameflow
