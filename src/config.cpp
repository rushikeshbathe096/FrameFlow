#include "frameflow/config.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <system_error>

#include "frameflow/policy/policy_factory.hpp"

namespace frameflow {

namespace {

// Formats a limit without a trailing ".0" (e.g. 240, 0.5).
std::string number_text(double value) {
  char buffer[32];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

class Validator {
 public:
  explicit Validator(std::vector<ValidationError>& errors) : errors_(errors) {}

  void fail(const std::string& option, const std::string& value, const std::string& problem,
            const std::string& hint) {
    errors_.push_back({option, value, problem, hint});
  }

  // Number in [min, max]. Returns std::nullopt (and records an error) when invalid.
  std::optional<double> number(const std::optional<std::string>& text, const std::string& option,
                               double min, double max, const std::string& fallback) {
    if (!text) return std::nullopt;
    const std::optional<double> value = parse_number(*text);
    const std::string range =
        "must be a number between " + number_text(min) + " and " + number_text(max);
    if (!value || *value < min || *value > max) {
      fail(option, *text, range, "try " + option + "=" + fallback);
      return std::nullopt;
    }
    return value;
  }

  std::optional<std::uint64_t> integer(const std::optional<std::string>& text,
                                       const std::string& option, std::uint64_t min,
                                       std::uint64_t max, const std::string& fallback) {
    if (!text) return std::nullopt;
    const std::optional<std::uint64_t> value = parse_unsigned(*text);
    if (!value || *value < min || *value > max) {
      fail(option, *text,
           "must be a whole number between " + std::to_string(min) + " and " + std::to_string(max),
           "try " + option + "=" + fallback);
      return std::nullopt;
    }
    return value;
  }

  // One of `allowed` (case-sensitive).
  std::optional<std::string> choice(const std::optional<std::string>& text,
                                    const std::string& option,
                                    const std::vector<std::string_view>& allowed) {
    if (!text) return std::nullopt;
    if (std::find(allowed.begin(), allowed.end(), *text) != allowed.end()) return *text;
    std::string list;
    for (std::string_view name : allowed) list += (list.empty() ? "" : ", ") + std::string(name);
    fail(option, *text, "must be one of: " + list, "try " + option + "=" + std::string(allowed[0]));
    return std::nullopt;
  }

 private:
  std::vector<ValidationError>& errors_;
};

std::optional<cv::Size> parse_resolution(std::string_view text) {
  const std::size_t x = text.find('x');
  if (x == std::string_view::npos) return std::nullopt;
  const std::optional<std::uint64_t> w = parse_unsigned(text.substr(0, x));
  const std::optional<std::uint64_t> h = parse_unsigned(text.substr(x + 1));
  if (!w || !h || *w < 16 || *w > 7680 || *h < 16 || *h > 7680) return std::nullopt;
  return cv::Size(static_cast<int>(*w), static_cast<int>(*h));
}

void validate_run_length(const RawConfig& raw, Validator& v, RunConfig& config) {
  if (raw.duration && raw.frames) {  // VR-03: exactly one
    v.fail("--duration/--frames", "", "set only one of --duration and --frames",
           "remove one of them");
    return;
  }
  if (raw.frames) {
    if (const auto frames = v.integer(raw.frames, "--frames", 1, 1'000'000, "1800")) {
      config.frames = *frames;
      config.duration_s.reset();
    }
  } else if (const auto duration = v.number(raw.duration, "--duration", 1, 3600, "60")) {
    config.duration_s = *duration;
  }
}

void validate_warmup(const RawConfig& raw, Validator& v, RunConfig& config) {
  if (!raw.warmup) {
    if (config.warmup_s >= config.run_seconds()) config.warmup_s = 0.0;  // default never fails
    return;
  }
  const std::optional<double> warmup = parse_number(*raw.warmup);
  if (!warmup || *warmup < 0 || *warmup >= config.run_seconds()) {  // VR-04
    v.fail("--warmup", *raw.warmup,
           "must be at least 0 and less than the run length (" + number_text(config.run_seconds()) +
               " s)",
           "try --warmup=3");
    return;
  }
  config.warmup_s = *warmup;
}

void validate_source(const RawConfig& raw, Validator& v, RunConfig& config) {
  config.input = raw.input;
  if (raw.source) {
    const auto source = v.choice(raw.source, "--source", {"file", "synthetic"});
    if (!source) return;  // already reported; the --input rule only applies to a valid source
    config.source = *source;
  }
  if (config.source != "file") return;
  if (!config.input) {  // VR-15
    v.fail("--source", config.source, "needs --input=<video file>",
           "add --input or use --source=synthetic");
  } else {  // D20: the video source arrives in Phase 5
    v.fail("--source", config.source, "is not available yet", "use --source=synthetic");
  }
}

}  // namespace

std::optional<double> parse_number(std::string_view text) {
  double value = 0.0;
  const char* end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(text.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end || !std::isfinite(value)) return std::nullopt;
  return value;
}

std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
  std::uint64_t value = 0;
  const char* end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(text.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
  return value;
}

double RunConfig::run_seconds() const {
  return frames ? static_cast<double>(*frames) / fps : duration_s.value_or(0.0);
}

PipelineConfig RunConfig::to_pipeline_config() const {
  using std::chrono::duration;
  PipelineConfig out;
  out.fps = fps;
  out.queue_capacity = queue;
  out.cost = std::chrono::duration_cast<Clock::duration>(duration<double, std::milli>(cost_ms));
  out.cost_wait = cost_wait;
  out.policy = policy;
  if (frames) {
    out.frames = *frames;
  } else {
    out.duration = std::chrono::duration_cast<Clock::duration>(duration<double>(*duration_s));
  }
  return out;
}

ValidationResult validate(const RawConfig& raw) {
  ValidationResult result;
  RunConfig& config = result.config;
  Validator v(result.errors);

  if (const auto fps = v.number(raw.fps, "--fps", 1, 240, "30")) config.fps = *fps;  // VR-01
  if (const auto queue = v.integer(raw.queue, "--queue", 1, 1024, "8")) {            // VR-02
    config.queue = static_cast<std::size_t>(*queue);
  }
  validate_run_length(raw, v, config);                                         // VR-03
  validate_warmup(raw, v, config);                                             // VR-04
  if (const auto cost = v.number(raw.cost_ms, "--cost-ms", 0, 10'000, "0")) {  // VR-05
    config.cost_ms = *cost;
  }
  if (const auto mode = v.choice(raw.cost_mode, "--cost-mode", {"fixed"})) config.cost_mode = *mode;
  if (const auto wait = v.choice(raw.cost_wait, "--cost-wait", {"busy", "sleep"})) {
    config.cost_wait = *wait == "sleep" ? CostWait::Sleep : CostWait::Busy;
  }
  if (const auto policy = v.choice(raw.policy, "--policy", policy_names())) {  // VR-07
    config.policy = *policy;
  }
  if (raw.resolution) {  // VR-13
    if (const std::optional<cv::Size> size = parse_resolution(*raw.resolution)) {
      config.resolution = *size;
    } else {
      v.fail("--resolution", *raw.resolution, "must be WxH with each side between 16 and 7680",
             "try --resolution=1280x720");
    }
  }
  validate_source(raw, v, config);  // VR-15
  if (const auto seed = v.integer(raw.seed, "--seed", 0, UINT64_MAX, "42")) config.seed = *seed;
  return result;
}

std::string format_validation_errors(const std::vector<ValidationError>& errors) {
  std::size_t option_width = 0;
  std::size_t problem_width = 0;
  for (const ValidationError& e : errors) {
    const std::size_t shown = e.option.size() + (e.value.empty() ? 0 : 1 + e.value.size());
    option_width = std::max(option_width, shown);
    problem_width = std::max(problem_width, e.problem.size());
  }
  std::string out = "frameflow: error: " + std::to_string(errors.size()) +
                    (errors.size() == 1 ? " invalid option\n" : " invalid options\n");
  for (const ValidationError& e : errors) {
    std::string shown = e.option + (e.value.empty() ? "" : " " + e.value);
    shown.resize(option_width, ' ');
    std::string problem = e.problem;
    problem.resize(problem_width, ' ');
    out += "  " + shown + " " + problem + " (hint: " + e.hint + ")\n";
  }
  out += "Run 'frameflow run --help' to see all options.\n";
  return out;
}

}  // namespace frameflow
