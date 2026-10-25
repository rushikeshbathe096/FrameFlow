#include "frameflow/cli.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include "frameflow/version.hpp"

namespace frameflow {
namespace {

struct Parsed {
  CliParse result;
  std::string out;
  std::string err;
};

Parsed parse(std::vector<const char*> args) {
  args.insert(args.begin(), "frameflow");
  std::ostringstream out;
  std::ostringstream err;
  Parsed parsed{parse_cli(static_cast<int>(args.size()), args.data(), out, err), "", ""};
  parsed.out = out.str();
  parsed.err = err.str();
  return parsed;
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

TEST(CliTest, RunFillsRawOptionsInBothSyntaxes) {
  const Parsed p = parse({"run", "--fps=60", "--queue", "4", "--policy=drop-oldest",
                          "--source=synthetic", "--frames=100", "--out=x/y"});
  ASSERT_TRUE(p.result.run.has_value()) << p.err;
  const RawConfig& raw = p.result.run->raw;
  EXPECT_EQ(raw.fps, "60");
  EXPECT_EQ(raw.queue, "4");
  EXPECT_EQ(raw.policy, "drop-oldest");
  EXPECT_EQ(raw.source, "synthetic");
  EXPECT_EQ(raw.frames, "100");
  EXPECT_EQ(p.result.run->out, "x/y");
  EXPECT_EQ(p.err, "");
}

TEST(CliTest, UnsetOptionsStayUnsetSoValidationAppliesDefaults) {
  const Parsed p = parse({"run"});
  ASSERT_TRUE(p.result.run.has_value());
  const RawConfig& raw = p.result.run->raw;
  EXPECT_FALSE(raw.fps.has_value());
  EXPECT_FALSE(raw.duration.has_value());
  EXPECT_FALSE(raw.policy.has_value());
  EXPECT_FALSE(p.result.run->out.has_value());
}

TEST(CliTest, ValuesAreNotValidatedByTheParser) {
  // Validation (config.cpp) reports every bad value at once; the parser passes them through.
  const Parsed p = parse({"run", "--fps=abc", "--queue=0"});
  ASSERT_TRUE(p.result.run.has_value());
  EXPECT_EQ(p.result.run->raw.fps, "abc");
}

TEST(CliTest, VersionShowsVersionAndCommit) {
  const Parsed p = parse({"--version"});
  EXPECT_FALSE(p.result.run.has_value());
  EXPECT_EQ(p.result.exit_code, kExitOk);
  EXPECT_TRUE(contains(p.out, version())) << p.out;
  EXPECT_TRUE(contains(p.out, git_commit())) << p.out;
}

TEST(CliTest, RootHelpListsRunAndExamples) {
  const Parsed p = parse({"--help"});
  EXPECT_FALSE(p.result.run.has_value());
  EXPECT_EQ(p.result.exit_code, kExitOk);
  EXPECT_TRUE(contains(p.out, "run")) << p.out;
  EXPECT_TRUE(contains(p.out, "Examples:"));
  EXPECT_TRUE(contains(p.out, "frameflow run --help"));
}

TEST(CliTest, RunHelpListsEveryOptionWithDefault) {
  const Parsed p = parse({"run", "--help"});
  EXPECT_EQ(p.result.exit_code, kExitOk);
  const std::vector<std::pair<const char*, const char*>> flags = {
      {"--source", "[file]"},     {"--input", "[none]"},
      {"--fps", "[30]"},          {"--resolution", "[1280x720]"},
      {"--cost-mode", "[fixed]"}, {"--cost-ms", "[0]"},
      {"--cost-wait", "[busy]"},  {"--queue", "[8]"},
      {"--policy", "[blocking]"}, {"--duration", "[60]"},
      {"--frames", "[none]"},     {"--warmup", "[3]"},
      {"--seed", "[42]"},         {"--out", "[results/run_NNN]"}};
  for (const auto& [flag, default_text] : flags) {
    EXPECT_TRUE(contains(p.out, flag)) << flag;
    EXPECT_TRUE(contains(p.out, default_text)) << default_text;
  }
  for (const char* group : {"Source", "Processing", "Queue and policy", "Run control", "Output"}) {
    EXPECT_TRUE(contains(p.out, group)) << group;
  }
}

TEST(CliTest, UnknownFlagSuggestsClosest) {
  const Parsed p = parse({"run", "--polcy=drop-oldest"});
  EXPECT_FALSE(p.result.run.has_value());
  EXPECT_EQ(p.result.exit_code, kExitUsage);
  EXPECT_EQ(p.err, "frameflow: error: unknown option: --polcy (hint: did you mean --policy?)\n");
}

TEST(CliTest, EveryUnknownFlagIsReported) {
  const Parsed p = parse({"run", "--fsp=30", "--queu", "4"});
  EXPECT_EQ(p.result.exit_code, kExitUsage);
  EXPECT_TRUE(contains(p.err, "--fsp (hint: did you mean --fps?)")) << p.err;
  EXPECT_TRUE(contains(p.err, "--queu (hint: did you mean --queue?)")) << p.err;
}

TEST(CliTest, MissingCommandIsUsageError) {
  const Parsed p = parse({});
  EXPECT_EQ(p.result.exit_code, kExitUsage);
  EXPECT_TRUE(contains(p.err, "frameflow: error: ")) << p.err;
}

TEST(CliTest, UnknownCommandIsUsageError) {
  const Parsed p = parse({"rnu"});
  EXPECT_EQ(p.result.exit_code, kExitUsage);
  EXPECT_TRUE(contains(p.err, "rnu")) << p.err;
}

TEST(CliTest, MissingValueIsUsageError) {
  const Parsed p = parse({"run", "--fps"});
  EXPECT_EQ(p.result.exit_code, kExitUsage);
  EXPECT_TRUE(contains(p.err, "--fps")) << p.err;
}

TEST(CliTest, ClosestFlagPicksSmallestEditDistance) {
  const std::vector<std::string> known = {"--fps", "--frames", "--policy", "--queue"};
  EXPECT_EQ(closest_flag("--polcy", known), "--policy");
  EXPECT_EQ(closest_flag("--frame", known), "--frames");
  EXPECT_EQ(closest_flag("--fp", known), "--fps");
  EXPECT_EQ(closest_flag("--x", {}), "");
}

TEST(CliTest, RunStartShowsEffectiveConfiguration) {
  RunConfig config;
  config.source = "synthetic";
  config.cost_ms = 50.0;
  const std::string expected = std::string("FrameFlow ") + version() +
                               "  ·  run started\n"
                               "  source     synthetic 1280x720 @ 30 fps\n"
                               "  processing cost 50 ms (fixed, busy)  ·  overload 1.50x\n"
                               "  queue      8 frames  ·  policy blocking\n"
                               "  run        60 s (warm-up 3 s)  ·  seed 42\n"
                               "  output     results/run_001/\n";
  EXPECT_EQ(format_run_start(config, "results/run_001"), expected);
}

TEST(CliTest, RunStartShowsFrameCountWhenRunIsByFrames) {
  RunConfig config;
  config.duration_s.reset();
  config.frames = 300;
  config.fps = 29.97;
  config.warmup_s = 0.5;
  const std::string text = format_run_start(config, "out");
  EXPECT_TRUE(contains(text, "@ 29.97 fps")) << text;
  EXPECT_TRUE(contains(text, "run        300 frames (warm-up 0.5 s)")) << text;
}

}  // namespace
}  // namespace frameflow
