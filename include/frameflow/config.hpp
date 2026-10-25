// config.hpp
// Responsibility: turns raw option strings (from CLI flags now, YAML later) into a validated
// RunConfig (SRS section 7). Numbers are parsed strictly (VR-20: no trailing characters, NaN,
// infinity or overflow), every rule is checked, and all problems are reported together (VR-23) in
// the UIUX 8.2 message format.
// Thread-safety: pure functions and plain data; validation runs on the main thread before any
// pipeline thread starts.
#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "frameflow/clock.hpp"
#include "frameflow/pipeline.hpp"
#include "frameflow/processor/cost_model.hpp"

namespace frameflow {

// Unvalidated option values exactly as the user typed them; unset means "use the default".
struct RawConfig {
  std::optional<std::string> fps, queue, duration, frames, warmup, cost_ms, cost_mode, cost_wait,
      policy, resolution, source, input, seed;
};

struct RunConfig {
  double fps = 30.0;
  std::size_t queue = 8;
  std::optional<double> duration_s = 60.0;  // exactly one of duration_s / frames is set
  std::optional<std::uint64_t> frames;
  double warmup_s = 3.0;
  double cost_ms = 0.0;
  std::string cost_mode = "fixed";
  CostWait cost_wait = CostWait::Busy;
  std::string policy = "blocking";
  cv::Size resolution{1280, 720};
  std::string source = "file";
  std::optional<std::string> input;
  std::uint64_t seed = 42;

  double run_seconds() const;  // duration, or frames / fps
  PipelineConfig to_pipeline_config() const;
};

struct ValidationError {
  std::string option;   // e.g. "--queue"
  std::string value;    // as given, or "" when the problem is a missing/conflicting option
  std::string problem;  // e.g. "must be between 1 and 1024"
  std::string hint;     // e.g. "try --queue=8"
};

struct ValidationResult {
  RunConfig config;  // meaningful only when errors is empty
  std::vector<ValidationError> errors;
  bool ok() const { return errors.empty(); }
};

ValidationResult validate(const RawConfig& raw);

// UIUX 8.2: "frameflow: error: N invalid options" + one aligned line per error + help pointer.
std::string format_validation_errors(const std::vector<ValidationError>& errors);

// Strict parsers (VR-20). std::nullopt for anything that is not exactly one finite number.
std::optional<double> parse_number(std::string_view text);
std::optional<std::uint64_t> parse_unsigned(std::string_view text);

}  // namespace frameflow
