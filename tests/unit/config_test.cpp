#include "frameflow/config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace frameflow {
namespace {

// Synthetic source so that only the option under test can fail (the default source is `file`).
RawConfig synthetic() {
  RawConfig raw;
  raw.source = "synthetic";
  return raw;
}

ValidationResult with(std::optional<std::string> RawConfig::*field, const std::string& value) {
  RawConfig raw = synthetic();
  raw.*field = value;
  return validate(raw);
}

void expect_accepted(std::optional<std::string> RawConfig::*field, const std::string& value) {
  const ValidationResult result = with(field, value);
  EXPECT_TRUE(result.ok()) << value << ": " << format_validation_errors(result.errors);
}

void expect_rejected(std::optional<std::string> RawConfig::*field, const std::string& value,
                     const std::string& option) {
  const ValidationResult result = with(field, value);
  ASSERT_EQ(result.errors.size(), 1u) << value;
  EXPECT_EQ(result.errors[0].option, option);
  EXPECT_EQ(result.errors[0].value, value);
}

TEST(ConfigTest, DefaultsMatchTheSrs) {
  const ValidationResult result = validate(synthetic());
  ASSERT_TRUE(result.ok());
  const RunConfig& c = result.config;
  EXPECT_EQ(c.fps, 30.0);
  EXPECT_EQ(c.queue, 8u);
  EXPECT_EQ(c.duration_s, 60.0);
  EXPECT_FALSE(c.frames.has_value());
  EXPECT_EQ(c.warmup_s, 3.0);
  EXPECT_EQ(c.cost_ms, 0.0);
  EXPECT_EQ(c.cost_wait, CostWait::Busy);
  EXPECT_EQ(c.policy, "blocking");
  EXPECT_EQ(c.resolution, cv::Size(1280, 720));
  EXPECT_EQ(c.seed, 42u);
}

// VR-01 --fps: number, 1 to 240.
TEST(ConfigTest, Fps) {
  for (const char* ok : {"1", "30", "29.97", "240"}) expect_accepted(&RawConfig::fps, ok);
  for (const char* bad : {"0", "240.5", "-5"}) expect_rejected(&RawConfig::fps, bad, "--fps");
}

// VR-02 --queue: integer, 1 to 1024.
TEST(ConfigTest, Queue) {
  for (const char* ok : {"1", "8", "1024"}) expect_accepted(&RawConfig::queue, ok);
  for (const char* bad : {"0", "1025", "8.0", "-1"}) {
    expect_rejected(&RawConfig::queue, bad, "--queue");
  }
}

// VR-03 --duration / --frames: exactly one; duration 1-3600 s; frames 1-1,000,000.
TEST(ConfigTest, DurationAndFrames) {
  for (const char* ok : {"1", "60", "3600", "2.5"}) expect_accepted(&RawConfig::duration, ok);
  for (const char* bad : {"0", "3601"}) expect_rejected(&RawConfig::duration, bad, "--duration");
  for (const char* ok : {"100", "1000000"}) expect_accepted(&RawConfig::frames, ok);
  for (const char* bad : {"0", "1000001", "1.5"}) {
    expect_rejected(&RawConfig::frames, bad, "--frames");
  }

  RawConfig both = synthetic();
  both.duration = "10";
  both.frames = "100";
  const ValidationResult result = validate(both);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].option, "--duration/--frames");
}

TEST(ConfigTest, FramesReplaceTheDefaultDuration) {
  const ValidationResult result = with(&RawConfig::frames, "300");
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.config.duration_s.has_value());
  EXPECT_EQ(result.config.frames, 300u);
  EXPECT_DOUBLE_EQ(result.config.run_seconds(), 10.0);  // 300 frames at 30 FPS
}

// VR-04 --warmup: seconds, 0 to less than the run length.
TEST(ConfigTest, Warmup) {
  for (const char* ok : {"0", "3", "59.9"}) expect_accepted(&RawConfig::warmup, ok);
  for (const char* bad : {"-1", "60", "100"}) expect_rejected(&RawConfig::warmup, bad, "--warmup");
}

TEST(ConfigTest, DefaultWarmupNeverInvalidatesAShortRun) {
  const ValidationResult result = with(&RawConfig::duration, "2");  // D21
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.config.warmup_s, 0.0);
}

// VR-05 --cost-ms: number, 0 to 10,000.
TEST(ConfigTest, CostMs) {
  for (const char* ok : {"0", "50", "33.3", "10000"}) expect_accepted(&RawConfig::cost_ms, ok);
  for (const char* bad : {"-0.1", "10001"}) expect_rejected(&RawConfig::cost_ms, bad, "--cost-ms");
}

TEST(ConfigTest, CostModeAndWait) {
  expect_accepted(&RawConfig::cost_mode, "fixed");
  expect_rejected(&RawConfig::cost_mode, "FIXED", "--cost-mode");
  expect_rejected(&RawConfig::cost_mode, "spiky", "--cost-mode");
  expect_accepted(&RawConfig::cost_wait, "busy");
  expect_accepted(&RawConfig::cost_wait, "sleep");
  expect_rejected(&RawConfig::cost_wait, "spin", "--cost-wait");
  EXPECT_EQ(with(&RawConfig::cost_wait, "sleep").config.cost_wait, CostWait::Sleep);
}

RawConfig bursty(const std::string& spike_ms, const std::string& len, const std::string& every) {
  RawConfig raw = synthetic();
  raw.cost_mode = "bursty";
  raw.cost_ms = "20";
  raw.spike_ms = spike_ms;
  raw.spike_len = len;
  raw.spike_every = every;
  return raw;
}

// VR-06: bursty needs all three spike flags; spike-ms >= cost-ms; spike-len < spike-every.
TEST(ConfigTest, BurstyAcceptsCompleteSchedule) {
  const ValidationResult result = validate(bursty("80", "1s", "5s"));
  ASSERT_TRUE(result.ok()) << format_validation_errors(result.errors);
  EXPECT_EQ(result.config.cost_mode, "bursty");
  EXPECT_EQ(result.config.spike_ms, 80.0);
  EXPECT_EQ(result.config.spike_len_s, 1.0);
  EXPECT_EQ(result.config.spike_every_s, 5.0);
  EXPECT_TRUE(validate(bursty("20", "500ms", "2")).ok());  // ms suffix, bare seconds, equal cost
}

TEST(ConfigTest, BurstyRequiresAllThreeSpikeFlags) {
  RawConfig raw = bursty("80", "1s", "5s");
  raw.spike_every.reset();
  const ValidationResult result = validate(raw);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].option, "--cost-mode");
}

TEST(ConfigTest, SpikeCostBelowBaseCostIsRejected) {
  const ValidationResult result = validate(bursty("10", "1s", "5s"));
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].option, "--spike-ms");
}

TEST(ConfigTest, SpikeLengthMustBeShorterThanPeriod) {
  const ValidationResult result = validate(bursty("80", "5s", "5s"));
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].option, "--spike-len");
}

TEST(ConfigTest, SpikeDurationsMustParse) {
  for (const char* bad : {"1sec", "0s", "-1s", "abc", "4000s"}) {
    const ValidationResult result = validate(bursty("80", bad, "5s"));
    ASSERT_FALSE(result.ok()) << bad;
    EXPECT_EQ(result.errors[0].option, "--spike-len") << bad;
  }
}

TEST(ConfigTest, SpikeFlagsWithoutBurstyAreRejected) {
  RawConfig raw = synthetic();
  raw.spike_ms = "80";
  const ValidationResult result = validate(raw);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].option, "--spike-ms/--spike-len/--spike-every");
}

TEST(ConfigTest, BurstyConvertsToPipelineBurstWithSeed) {
  RawConfig raw = bursty("80", "1s", "5s");
  raw.seed = "7";
  const PipelineConfig p = validate(raw).config.to_pipeline_config();
  ASSERT_TRUE(p.burst.has_value());
  EXPECT_EQ(p.burst->spike_cost, std::chrono::milliseconds(80));
  EXPECT_EQ(p.burst->length, std::chrono::seconds(1));
  EXPECT_EQ(p.burst->every, std::chrono::seconds(5));
  EXPECT_EQ(p.burst->seed, 7u);
  EXPECT_FALSE(validate(synthetic()).config.to_pipeline_config().burst.has_value());
}

TEST(ConfigTest, ParseSeconds) {
  EXPECT_EQ(parse_seconds("1s"), 1.0);
  EXPECT_EQ(parse_seconds("250ms"), 0.25);
  EXPECT_EQ(parse_seconds("2.5"), 2.5);
  EXPECT_EQ(parse_seconds("s"), std::nullopt);
  EXPECT_EQ(parse_seconds("ms"), std::nullopt);
  EXPECT_EQ(parse_seconds("1 s"), std::nullopt);
}

RawConfig adaptive() {
  RawConfig raw = synthetic();
  raw.policy = "adaptive";
  return raw;
}

TEST(ConfigTest, AdaptiveDefaults) {
  const ValidationResult r = validate(adaptive());
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.config.target_ms, 100.0);
  EXPECT_EQ(r.config.exit_ratio, 0.7);
  EXPECT_EQ(r.config.window, 30u);
  const PipelineConfig p = r.config.to_pipeline_config();
  EXPECT_EQ(p.adaptive.target, std::chrono::milliseconds(100));
  EXPECT_EQ(p.adaptive.exit_ratio, 0.7);
  EXPECT_EQ(p.adaptive.window, 30u);
  EXPECT_FALSE(validate(synthetic()).config.target_ms.has_value());  // only with adaptive
}

void expect_adaptive(std::optional<std::string> RawConfig::*field, const std::string& value,
                     bool ok, const std::string& option) {
  RawConfig raw = adaptive();
  raw.*field = value;
  const ValidationResult r = validate(raw);
  if (ok) {
    EXPECT_TRUE(r.ok()) << value << ": " << format_validation_errors(r.errors);
  } else {
    ASSERT_EQ(r.errors.size(), 1u) << value;
    EXPECT_EQ(r.errors[0].option, option) << value;
  }
}

// VR-08 --target-ms: 1 to 60000.
TEST(ConfigTest, TargetMs) {
  for (const char* ok : {"1", "100", "60000", "33.5"}) {
    expect_adaptive(&RawConfig::target_ms, ok, true, "");
  }
  for (const char* bad : {"0", "60001", "-5", "abc"}) {
    expect_adaptive(&RawConfig::target_ms, bad, false, "--target-ms");
  }
}

// VR-09 --exit-ratio: greater than 0 and less than 1.
TEST(ConfigTest, ExitRatio) {
  for (const char* ok : {"0.7", "0.01", "0.99"}) {
    expect_adaptive(&RawConfig::exit_ratio, ok, true, "");
  }
  for (const char* bad : {"0", "1", "1.2", "-0.5", "x"}) {
    expect_adaptive(&RawConfig::exit_ratio, bad, false, "--exit-ratio");
  }
}

// VR-10 --window: integer 5 to 1000.
TEST(ConfigTest, Window) {
  for (const char* ok : {"5", "30", "1000"}) expect_adaptive(&RawConfig::window, ok, true, "");
  for (const char* bad : {"4", "1001", "10.5", ""}) {
    expect_adaptive(&RawConfig::window, bad, false, "--window");
  }
}

// VR-08: "only with adaptive".
TEST(ConfigTest, AdaptiveOptionsRequireAdaptivePolicy) {
  RawConfig raw = synthetic();
  raw.policy = "blocking";
  raw.target_ms = "100";
  const ValidationResult r = validate(raw);
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_EQ(r.errors[0].option, "--target-ms/--exit-ratio/--window");
  raw.policy = "leaky";  // an invalid policy is reported alone, without the "only with" error
  const ValidationResult invalid = validate(raw);
  ASSERT_EQ(invalid.errors.size(), 1u);
  EXPECT_EQ(invalid.errors[0].option, "--policy");
}

// VR-11 --processor: none, grayscale, blur, canny.
TEST(ConfigTest, Processor) {
  for (const char* ok : {"none", "grayscale", "blur", "canny"}) {
    expect_accepted(&RawConfig::processor, ok);
  }
  for (const char* bad : {"Blur", "sharpen", ""}) {
    expect_rejected(&RawConfig::processor, bad, "--processor");
  }
  EXPECT_EQ(with(&RawConfig::processor, "canny").config.processor, "canny");
  EXPECT_EQ(validate(synthetic()).config.processor, "none");
}

// VR-12 --kernel: odd integer 3 to 99.
TEST(ConfigTest, Kernel) {
  for (const char* ok : {"3", "15", "99"}) expect_accepted(&RawConfig::kernel, ok);
  for (const char* bad : {"2", "16", "1", "101", "7.0", "x"}) {
    expect_rejected(&RawConfig::kernel, bad, "--kernel");
  }
  const PipelineConfig p = with(&RawConfig::kernel, "21").config.to_pipeline_config();
  EXPECT_EQ(p.kernel, 21);
}

// VR-07 --policy: one of the registered names, case-sensitive.
TEST(ConfigTest, Policy) {
  for (const char* ok : {"blocking", "drop-oldest", "drop-newest", "adaptive"}) {
    expect_accepted(&RawConfig::policy, ok);
  }
  for (const char* bad : {"Blocking", "leaky", ""}) {
    expect_rejected(&RawConfig::policy, bad, "--policy");
  }
}

// VR-13 --resolution: WxH, each 16 to 7680.
TEST(ConfigTest, Resolution) {
  for (const char* ok : {"16x16", "1280x720", "7680x4320"}) {
    expect_accepted(&RawConfig::resolution, ok);
  }
  for (const char* bad : {"15x720", "1280x7681", "1280", "1280x", "x720", "1280X720", "12a0x720"}) {
    expect_rejected(&RawConfig::resolution, bad, "--resolution");
  }
  EXPECT_EQ(with(&RawConfig::resolution, "640x480").config.resolution, cv::Size(640, 480));
}

// VR-15 --source: file or synthetic; file needs --input. The file is checked when opened (D48).
TEST(ConfigTest, Source) {
  expect_accepted(&RawConfig::source, "synthetic");
  expect_rejected(&RawConfig::source, "camera", "--source");

  RawConfig file_without_input;  // the default source is `file`
  const ValidationResult missing = validate(file_without_input);
  ASSERT_EQ(missing.errors.size(), 1u);
  EXPECT_EQ(missing.errors[0].problem, "needs --input=<video file>");

  RawConfig file_with_input;
  file_with_input.input = "clip.mp4";
  const ValidationResult with_input = validate(file_with_input);
  EXPECT_TRUE(with_input.ok());
  EXPECT_EQ(with_input.config.input, "clip.mp4");
  EXPECT_TRUE(with_input.config.loop);  // FR-SRC-06 default
}

TEST(ConfigTest, Loop) {
  EXPECT_FALSE(with(&RawConfig::loop, "false").config.loop);
  EXPECT_TRUE(with(&RawConfig::loop, "true").config.loop);
  expect_rejected(&RawConfig::loop, "no", "--loop");
}

// VR-16 --seed: unsigned 64-bit integer.
TEST(ConfigTest, Seed) {
  for (const char* ok : {"0", "42", "18446744073709551615"}) expect_accepted(&RawConfig::seed, ok);
  for (const char* bad : {"-1", "18446744073709551616", "4.2"}) {
    expect_rejected(&RawConfig::seed, bad, "--seed");
  }
}

// VR-20: numeric options reject NaN, infinity, trailing characters and overflow.
TEST(ConfigTest, StrictNumberParsing) {
  for (const char* bad : {"nan", "NaN", "inf", "-inf", "30fps", " 30", "30 ", "", "1e999", "+30"}) {
    EXPECT_FALSE(parse_number(bad).has_value()) << '"' << bad << '"';
    expect_rejected(&RawConfig::fps, bad, "--fps");
  }
  EXPECT_EQ(parse_number("29.97"), 29.97);
  EXPECT_EQ(parse_number("1e2"), 100.0);
  EXPECT_FALSE(parse_unsigned("99999999999999999999").has_value());  // overflow
  EXPECT_FALSE(parse_unsigned("8x").has_value());
}

// VR-23: every invalid option is reported, not only the first.
TEST(ConfigTest, ReportsAllErrorsAtOnce) {
  RawConfig raw = synthetic();
  raw.queue = "0";
  raw.fps = "fast";
  raw.policy = "leaky";
  const ValidationResult result = validate(raw);
  ASSERT_EQ(result.errors.size(), 3u);
  EXPECT_EQ(result.errors[0].option, "--fps");
  EXPECT_EQ(result.errors[1].option, "--queue");
  EXPECT_EQ(result.errors[2].option, "--policy");
}

// UIUX 8.2 layout: count line, aligned columns, hint in brackets, help pointer.
TEST(ConfigTest, FormatsErrorsLikeUiux) {
  RawConfig raw = synthetic();
  raw.queue = "0";
  raw.fps = "300";
  const std::string message = format_validation_errors(validate(raw).errors);
  EXPECT_EQ(message,
            "frameflow: error: 2 invalid options\n"
            "  --fps 300 must be a number between 1 and 240        (hint: try --fps=30)\n"
            "  --queue 0 must be a whole number between 1 and 1024 (hint: try --queue=8)\n"
            "Run 'frameflow run --help' to see all options.\n");
}

TEST(ConfigTest, SingleErrorIsSingular) {
  RawConfig raw = synthetic();
  raw.queue = "0";
  EXPECT_EQ(format_validation_errors(validate(raw).errors)
                .rfind("frameflow: error: 1 invalid option\n", 0),
            0u);
}

TEST(ConfigTest, ConvertsToPipelineConfig) {
  RawConfig raw = synthetic();
  raw.fps = "25";
  raw.queue = "4";
  raw.cost_ms = "12.5";
  raw.policy = "drop-oldest";
  raw.duration = "2";
  const ValidationResult result = validate(raw);
  ASSERT_TRUE(result.ok());
  const PipelineConfig p = result.config.to_pipeline_config();
  EXPECT_EQ(p.fps, 25.0);
  EXPECT_EQ(p.queue_capacity, 4u);
  EXPECT_EQ(p.cost, std::chrono::microseconds(12500));
  EXPECT_EQ(p.policy, "drop-oldest");
  EXPECT_EQ(p.duration, std::chrono::seconds(2));
  EXPECT_FALSE(p.frames.has_value());
}

}  // namespace
}  // namespace frameflow
