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
  expect_accepted(&RawConfig::cost_wait, "busy");
  expect_accepted(&RawConfig::cost_wait, "sleep");
  expect_rejected(&RawConfig::cost_wait, "spin", "--cost-wait");
  EXPECT_EQ(with(&RawConfig::cost_wait, "sleep").config.cost_wait, CostWait::Sleep);
}

// VR-07 --policy: one of the registered names, case-sensitive.
TEST(ConfigTest, Policy) {
  for (const char* ok : {"blocking", "drop-oldest", "drop-newest"}) {
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

// VR-15 --source: file or synthetic; file needs --input (and is not available until Phase 5).
TEST(ConfigTest, Source) {
  expect_accepted(&RawConfig::source, "synthetic");
  expect_rejected(&RawConfig::source, "camera", "--source");

  RawConfig file_without_input;  // the default source is `file`
  const ValidationResult missing = validate(file_without_input);
  ASSERT_EQ(missing.errors.size(), 1u);
  EXPECT_EQ(missing.errors[0].problem, "needs --input=<video file>");

  RawConfig file_with_input;
  file_with_input.input = "clip.mp4";
  const ValidationResult unavailable = validate(file_with_input);
  ASSERT_EQ(unavailable.errors.size(), 1u);
  EXPECT_EQ(unavailable.errors[0].problem, "is not available yet");  // D20
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
