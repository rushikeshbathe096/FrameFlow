#include "frameflow/cli.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <charconv>
#include <exception>
#include <sstream>

#include "frameflow/platform/privileges.hpp"
#include "frameflow/run_command.hpp"
#include "frameflow/version.hpp"

namespace frameflow {

namespace {

// Shortest round-trip form without the locale, e.g. 30, 29.97 (DR-07 style).
std::string number_text(double value) {
  char buffer[32];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

std::string fixed2(double value) {
  char buffer[48];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 2);
  return std::string(buffer, result.ptr);
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
      {{"--cost-mode", "<fixed>", "processing cost model", "fixed", "Processing"}, &r.cost_mode},
      {{"--cost-ms", "<0-10000>", "processing cost per frame (ms)", "0", "Processing"}, &r.cost_ms},
      {{"--cost-wait", "<busy|sleep>", "how the cost is spent", "busy", "Processing"},
       &r.cost_wait},
      {{"--queue", "<1-1024>", "queue capacity (frames)", "8", "Queue and policy"}, &r.queue},
      {{"--policy", "<name>", "blocking, drop-oldest or drop-newest", "blocking",
        "Queue and policy"},
       &r.policy},
      {{"--duration", "<1-3600>", "run length (s)", "60", "Run control"}, &r.duration},
      {{"--frames", "<1-1000000>", "run length in frames instead of --duration", "none",
        "Run control"},
       &r.frames},
      {{"--warmup", "<seconds>", "start of the run excluded from statistics", "3", "Run control"},
       &r.warmup},
      {{"--seed", "<uint64>", "random seed", "42", "Run control"}, &r.seed},
      {{"--out", "<dir>", "output folder (must not exist)", "results/run_NNN", "Output"},
       &args.out},
  };
  for (const auto& [spec, target] : flags) {
    run.add_option(spec.flag, *target, spec.help)
        ->type_name(spec.value)
        ->default_str(spec.default_text)
        ->group(spec.group);
  }
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

void configure_app(CLI::App& app, CLI::App& run) {
  app.name("frameflow");
  app.get_formatter()->column_width(40);
  app.description(std::string("FrameFlow ") + version() + " - latency-aware video pipeline");
  app.set_version_flag("--version", version_text(), "print version and git commit");
  app.require_subcommand(0, 1);  // a missing command is reported by parse_cli (clearer message)
  app.footer(
      "Examples:\n"
      "  frameflow run --source=synthetic --fps=30 --cost-ms=50 --policy=drop-oldest\n"
      "  frameflow run --source=synthetic --frames=300 --out=results/quick\n\n"
      "Run 'frameflow run --help' for all options.");
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
  CLI::App* run = app.add_subcommand("run", "one pipeline run");
  configure_app(app, *run);
  add_run_options(*run, args);

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
  if (!run->parsed()) {
    err << "frameflow: error: no command given (hint: try 'frameflow run --help')\n";
    result.exit_code = kExitUsage;
    return result;
  }
  if (!report_unknown_flags(*run, err)) {
    result.exit_code = kExitUsage;
    return result;
  }
  result.run = std::move(args);
  return result;
}

std::string format_run_start(const RunConfig& config, const std::filesystem::path& out_dir) {
  std::ostringstream text;
  const double overload = config.cost_ms * config.fps / 1000.0;
  const std::string length = config.frames ? std::to_string(*config.frames) + " frames"
                                           : number_text(config.duration_s.value_or(0.0)) + " s";
  text << "FrameFlow " << version() << "  ·  run started\n"
       << "  source     " << config.source << ' ' << config.resolution.width << 'x'
       << config.resolution.height << " @ " << number_text(config.fps) << " fps\n"
       << "  processing cost " << number_text(config.cost_ms) << " ms (" << config.cost_mode << ", "
       << (config.cost_wait == CostWait::Busy ? "busy" : "sleep") << ")  ·  overload "
       << fixed2(overload) << "x\n"
       << "  queue      " << config.queue << " frames  ·  policy " << config.policy << '\n'
       << "  run        " << length << " (warm-up " << number_text(config.warmup_s)
       << " s)  ·  seed " << config.seed << '\n'
       << "  output     " << out_dir.string() << "/\n";
  return text.str();
}

int cli_main(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
  try {
    const CliParse parsed = parse_cli(argc, argv, out, err);
    if (!parsed.run) return parsed.exit_code;
    return run_command(*parsed.run, effective_uid(), out, err);
  } catch (const std::exception& e) {
    err << "frameflow: error: internal error: " << e.what() << '\n';
    return kExitRuntime;
  }
}

}  // namespace frameflow
