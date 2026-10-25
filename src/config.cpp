#include "frameflow/config.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <system_error>

#include "frameflow/policy/policy_factory.hpp"
#include "frameflow/processor/processor.hpp"

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
    if (std::find(allowed.begin(), allowed.end(), *text) != allowed.end()) return text;
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
      config.frames = frames;
      config.duration_s.reset();
    }
  } else if (const auto duration = v.number(raw.duration, "--duration", 1, 3600, "60")) {
    config.duration_s = duration;
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
  // VR-15. The file itself (VR-14) is checked when it is opened: a bad file is exit 3, not 2.
  if (config.source == "file" && !config.input) {
    v.fail("--source", config.source, "needs --input=<video file>",
           "add --input or use --source=synthetic");
  }
  if (const auto loop = v.choice(raw.loop, "--loop", {"true", "false"})) {
    config.loop = *loop == "true";
  }
}

// Seconds in (0, max] from a duration flag (D39).
std::optional<double> spike_seconds(const std::optional<std::string>& text,
                                    const std::string& option, Validator& v,
                                    const std::string& fallback) {
  if (!text) return std::nullopt;
  const std::optional<double> seconds = parse_seconds(*text);
  if (!seconds || *seconds <= 0 || *seconds > 3600) {
    v.fail(option, *text, "must be a duration above 0 and at most 3600 s, like 1s or 500ms",
           "try " + option + "=" + fallback);
    return std::nullopt;
  }
  return seconds;
}

// VR-06: the three spike flags are required in bursty mode and only allowed there.
void validate_bursty(const RawConfig& raw, Validator& v, RunConfig& config) {
  const bool any = raw.spike_ms || raw.spike_len || raw.spike_every;
  if (config.cost_mode != "bursty") {
    if (any) {
      v.fail("--spike-ms/--spike-len/--spike-every", "", "only apply with --cost-mode=bursty",
             "add --cost-mode=bursty or remove them");
    }
    return;
  }
  if (!raw.spike_ms || !raw.spike_len || !raw.spike_every) {
    v.fail("--cost-mode", "bursty", "needs --spike-ms, --spike-len and --spike-every",
           "e.g. --spike-ms=80 --spike-len=1s --spike-every=5s");
    return;
  }
  const auto spike_ms = v.number(raw.spike_ms, "--spike-ms", 0, 10'000, "80");
  const auto len = spike_seconds(raw.spike_len, "--spike-len", v, "1s");
  const auto every = spike_seconds(raw.spike_every, "--spike-every", v, "5s");
  if (spike_ms && *spike_ms < config.cost_ms) {
    v.fail("--spike-ms", *raw.spike_ms,
           "must be at least --cost-ms (" + number_text(config.cost_ms) + ")",
           "raise --spike-ms or lower --cost-ms");
  }
  if (len && every && *len >= *every) {
    v.fail("--spike-len", *raw.spike_len,
           "must be shorter than --spike-every (" + *raw.spike_every + ")",
           "e.g. --spike-len=1s --spike-every=5s");
  }
  config.spike_ms = spike_ms;
  config.spike_len_s = len;
  config.spike_every_s = every;
}

// VR-08..VR-10: the three adaptive options, defaults 100 ms / 0.7 / 30, only with adaptive.
void validate_adaptive(const RawConfig& raw, bool policy_ok, Validator& v, RunConfig& config) {
  if (config.policy != "adaptive") {
    if (policy_ok && (raw.target_ms || raw.exit_ratio || raw.window)) {
      v.fail("--target-ms/--exit-ratio/--window", "", "only apply with --policy=adaptive",
             "add --policy=adaptive or remove them");
    }
    return;
  }
  config.target_ms = 100.0;
  config.exit_ratio = 0.7;
  config.window = 30;
  if (const auto target = v.number(raw.target_ms, "--target-ms", 1, 60'000, "100")) {
    config.target_ms = target;
  }
  if (raw.exit_ratio) {
    const std::optional<double> ratio = parse_number(*raw.exit_ratio);
    if (!ratio || *ratio <= 0.0 || *ratio >= 1.0) {
      v.fail("--exit-ratio", *raw.exit_ratio, "must be greater than 0 and less than 1",
             "0.7 is a good start");
    } else {
      config.exit_ratio = ratio;
    }
  }
  if (const auto window = v.integer(raw.window, "--window", 5, 1000, "30")) config.window = window;
}

}  // namespace

std::optional<double> parse_seconds(std::string_view text) {
  double scale = 1.0;
  if (text.size() > 2 && text.substr(text.size() - 2) == "ms") {
    text.remove_suffix(2);
    scale = 0.001;
  } else if (text.size() > 1 && text.back() == 's') {
    text.remove_suffix(1);
  }
  const std::optional<double> value = parse_number(text);
  if (!value) return std::nullopt;
  return *value * scale;
}

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
  out.processor = processor;
  out.kernel = kernel;
  if (target_ms && exit_ratio && window) {
    out.adaptive = AdaptiveParams{
        std::chrono::duration_cast<Clock::duration>(duration<double, std::milli>(*target_ms)),
        *exit_ratio, static_cast<std::size_t>(*window)};
  }
  if (spike_ms && spike_len_s && spike_every_s) {
    const auto to_clock = [](double seconds) {
      return std::chrono::duration_cast<Clock::duration>(duration<double>(seconds));
    };
    out.burst = BurstConfig{to_clock(*spike_ms / 1000.0), to_clock(*spike_len_s),
                            to_clock(*spike_every_s), seed};
  }
  if (frames) {
    out.frames = frames;
  } else {
    out.duration =
        std::chrono::duration_cast<Clock::duration>(duration<double>(duration_s.value_or(0.0)));
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
  if (const auto mode = v.choice(raw.cost_mode, "--cost-mode", {"fixed", "bursty"})) {
    config.cost_mode = *mode;
  }
  validate_bursty(raw, v, config);                                                         // VR-06
  if (const auto processor = v.choice(raw.processor, "--processor", processor_names())) {  // VR-11
    config.processor = *processor;
  }
  if (const auto kernel = v.integer(raw.kernel, "--kernel", 3, 99, "15")) {  // VR-12
    if (*kernel % 2 == 0) {
      v.fail("--kernel", raw.kernel.value_or(""), "must be odd",
             "try --kernel=" + std::to_string(*kernel + 1));
    } else {
      config.kernel = static_cast<int>(*kernel);
    }
  }
  if (const auto wait = v.choice(raw.cost_wait, "--cost-wait", {"busy", "sleep"})) {
    config.cost_wait = *wait == "sleep" ? CostWait::Sleep : CostWait::Busy;
  }
  const auto policy = v.choice(raw.policy, "--policy", policy_names());  // VR-07
  if (policy) config.policy = *policy;
  validate_adaptive(raw, !raw.policy || policy, v, config);  // VR-08..VR-10
  if (raw.resolution) {                                      // VR-13
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
