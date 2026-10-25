// cli.hpp
// Responsibility: the command line (UIUX 3, S1, S2). Parses argv with CLI11 into raw option
// strings (validation is config.hpp's job, so every problem is reported at once), prints
// `--help` and `--version`, rejects unknown flags with the closest valid one (VR-21), formats the
// effective-configuration block printed at run start (VR-24), and maps outcomes to the exit codes
// of SRS 9.1.
// Thread-safety: pure functions writing only to the streams they are given; called on the main
// thread before any pipeline thread starts.
#pragma once

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "frameflow/config.hpp"
#include "frameflow/exit_codes.hpp"

namespace frameflow {

// Everything `frameflow run` was given, still unvalidated.
struct RunArgs {
  RawConfig raw;                      // flags only; --config values are merged in run_command
  std::optional<std::string> config;  // --config: YAML file (VR-18)
  std::optional<std::string> out;
  bool no_loop = false;                // --no-loop: stop at the end of the video (sets raw.loop)
  std::optional<std::string> preview;  // --preview ("on") or --preview=auto (ER-06, D51)
  bool progress = false;               // --progress: status line once per second (S3, D52)
  bool overwrite = false;              // --overwrite: reuse an existing --out folder (D30)
  bool allow_root = false;             // --allow-root (FR-ROLE-02)
  bool allow_any_out = false;          // --allow-any-out (SR-05)
};

struct BenchArgs {
  std::string config;                  // --config (required)
  std::optional<std::string> repeats;  // --repeats (VR-19)
  std::optional<std::string> out;      // --out: parent of the experiment folder, default results
  bool overwrite = false;
  bool allow_root = false;
  bool allow_any_out = false;
};

struct CliParse {
  std::optional<RunArgs> run;      // set when `frameflow run` should execute
  std::optional<BenchArgs> bench;  // set when `frameflow bench` should execute
  int exit_code = kExitOk;         // the process exit code when run is empty
};

// Help and version text go to out, errors to err.
CliParse parse_cli(int argc, const char* const* argv, std::ostream& out, std::ostream& err);

// The known flag with the smallest edit distance to unknown (e.g. "--polcy" -> "--policy").
std::string closest_flag(std::string_view unknown, const std::vector<std::string>& known);

// UIUX S2: the effective configuration after defaults (VR-24). source_size: a video file's own
// frame size (the synthetic size comes from the config).
std::string format_run_start(const RunConfig& config, const std::filesystem::path& out_dir,
                             std::optional<cv::Size> source_size = std::nullopt);

// Parses argv and, for `run`, executes the run. Returns the process exit code.
int cli_main(int argc, const char* const* argv, std::ostream& out, std::ostream& err);

}  // namespace frameflow
