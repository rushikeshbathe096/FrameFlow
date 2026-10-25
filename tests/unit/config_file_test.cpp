#include "frameflow/config_file.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace frameflow {
namespace {

namespace fs = std::filesystem;

class ConfigFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("frameflow_cfg_test_" + std::to_string(::getpid()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path write(const std::string& name, const std::string& text) {
    const fs::path path = dir_ / name;
    std::ofstream(path, std::ios::binary) << text;
    return path;
  }

  fs::path dir_;
};

TEST_F(ConfigFileTest, ReadsTemplateStyleKeysAsRawStrings) {
  const fs::path path = write("run.yaml",
                              "source: { type: synthetic, resolution: 640x480, fps: 60 }\n"
                              "processor: { type: blur, kernel: 7 }\n"
                              "duration_s: 20\n"
                              "warmup_s: 2\n"
                              "seed: 7\n"
                              "cost_ms: 25.5\n"
                              "cost_mode: bursty\n"
                              "spike_ms: 80\n"
                              "spike_len: 1s\n"
                              "spike_every: 5s\n"
                              "queue: 4\n"
                              "policy: drop-oldest\n");
  const ConfigFileResult r = load_run_config(path);
  ASSERT_TRUE(r.ok()) << r.errors.front();
  EXPECT_EQ(r.raw.source, "synthetic");
  EXPECT_EQ(r.raw.resolution, "640x480");
  EXPECT_EQ(r.raw.fps, "60");
  EXPECT_EQ(r.raw.processor, "blur");
  EXPECT_EQ(r.raw.kernel, "7");
  EXPECT_EQ(r.raw.duration, "20");
  EXPECT_EQ(r.raw.warmup, "2");
  EXPECT_EQ(r.raw.seed, "7");
  EXPECT_EQ(r.raw.cost_ms, "25.5");
  EXPECT_EQ(r.raw.cost_mode, "bursty");
  EXPECT_EQ(r.raw.spike_len, "1s");
  EXPECT_EQ(r.raw.queue, "4");
  EXPECT_EQ(r.raw.policy, "drop-oldest");
  EXPECT_TRUE(validate(r.raw).ok());
}

TEST_F(ConfigFileTest, AdaptiveSection) {
  const ConfigFileResult r =
      load_run_config(write("run.yaml",
                            "source: {type: synthetic}\npolicy: adaptive\n"
                            "adaptive: { target_ms: 50, exit_ratio: 0.5, window: 10 }\n"));
  ASSERT_TRUE(r.ok()) << r.errors.front();
  EXPECT_EQ(r.raw.target_ms, "50");
  EXPECT_EQ(r.raw.exit_ratio, "0.5");
  EXPECT_EQ(r.raw.window, "10");
  EXPECT_TRUE(validate(r.raw).ok());
  const ConfigFileResult typo = load_run_config(write("typo.yaml", "adaptive: { target: 50 }\n"));
  ASSERT_EQ(typo.errors.size(), 1u);
  EXPECT_NE(typo.errors[0].find("did you mean 'target_ms'?"), std::string::npos) << typo.errors[0];
}

TEST_F(ConfigFileTest, UnknownKeyNamesFileLineColumnAndClosestKey) {
  const fs::path path = write("my_exp.yaml", "queue: 8\npolcy: blocking\n");
  const ConfigFileResult r = load_run_config(path);
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_EQ(r.errors[0], path.string() + ":2:1: unknown key 'polcy' (did you mean 'policy'?)");
}

TEST_F(ConfigFileTest, UnknownNestedKeySuggestsWithinSection) {
  const fs::path path = write("run.yaml", "source:\n  type: synthetic\n  fsp: 30\n");
  const ConfigFileResult r = load_run_config(path);
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_EQ(r.errors[0], path.string() + ":3:3: unknown key 'fsp' (did you mean 'fps'?)");
}

TEST_F(ConfigFileTest, ValuesAreValidatedLikeFlags) {
  const ConfigFileResult r = load_run_config(write("run.yaml", "queue: 0\nsource: {fps: 30fps}\n"));
  ASSERT_TRUE(r.ok());  // the file is well-formed; the values are checked by validate (VR-18)
  EXPECT_EQ(validate(r.raw).errors.size(), 3u);  // --queue, --fps, and --source=file needs input
}

TEST_F(ConfigFileTest, ExperimentKeysAreRefusedForRun) {
  const ConfigFileResult r = load_run_config(write("e.yaml", "repeats: 5\n"));
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_NE(r.errors[0].find("only allowed in experiment files"), std::string::npos) << r.errors[0];
}

TEST_F(ConfigFileTest, NonScalarValueIsRejected) {
  const ConfigFileResult r = load_run_config(write("run.yaml", "queue: [1, 2]\nfps_typo:\n"));
  ASSERT_EQ(r.errors.size(), 2u);
  EXPECT_NE(r.errors[0].find("'queue' must be a single value"), std::string::npos);
}

TEST_F(ConfigFileTest, EmptyValueIsRejected) {
  const ConfigFileResult r = load_run_config(write("run.yaml", "queue:\n"));
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_NE(r.errors[0].find("must be a single value"), std::string::npos);
}

TEST_F(ConfigFileTest, ProcessorValuesAreValidatedLikeFlags) {
  const ConfigFileResult r = load_run_config(
      write("run.yaml", "source: {type: synthetic}\nprocessor: { type: sharpen, kernel: 4 }\n"));
  ASSERT_TRUE(r.ok());  // well-formed; VR-11 and VR-12 are checked by validate()
  const ValidationResult v = validate(r.raw);
  ASSERT_EQ(v.errors.size(), 2u);
  EXPECT_EQ(v.errors[0].option, "--processor");
  EXPECT_EQ(v.errors[1].option, "--kernel");
}

TEST_F(ConfigFileTest, SyntaxErrorHasLineAndColumn) {
  const fs::path path = write("bad.yaml", "queue: 8\nsource: { fps: 30\n");
  const ConfigFileResult r = load_run_config(path);
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_EQ(r.errors[0].rfind(path.string() + ":", 0), 0u) << r.errors[0];
}

TEST_F(ConfigFileTest, MissingFileIsAnError) {
  const ConfigFileResult r = load_run_config(dir_ / "nope.yaml");
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_NE(r.errors[0].find("no such file"), std::string::npos);
}

TEST_F(ConfigFileTest, RootMustBeAMapping) {
  EXPECT_FALSE(load_run_config(write("list.yaml", "- 1\n- 2\n")).ok());
}

// SR-07: files above 1 MB are rejected before parsing.
TEST_F(ConfigFileTest, FileLargerThanOneMegabyteIsRejected) {
  std::string text = "queue: 8\n";
  text += "# " + std::string(kMaxConfigBytes, 'x') + "\n";
  const ConfigFileResult r = load_run_config(write("big.yaml", text));
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_NE(r.errors[0].find("larger than 1 MB"), std::string::npos);
}

// SR-07: a small file whose aliases expand beyond 1 MB ("billion laughs") is rejected.
TEST_F(ConfigFileTest, AliasExpansionBeyondOneMegabyteIsRejected) {
  std::string text = "a: &a [\"" + std::string(100, 'x') + "\"]\n";
  char prev = 'a';
  for (char c = 'b'; c <= 'k'; ++c) {  // each level repeats the previous one 10 times
    text += std::string(1, c) + ": &" + c + " [";
    for (int i = 0; i < 10; ++i) text += std::string(i ? ", *" : "*") + prev;
    text += "]\n";
    prev = c;
  }
  ASSERT_LT(text.size(), 2000u);
  ConfigErrors errors;
  EXPECT_FALSE(load_yaml_file(write("laughs.yaml", text), errors).has_value());
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("expands to more than 1 MB"), std::string::npos);
}

// FR-CLI-01, EC-19: a flag beats the file; unset flags keep the file's values.
TEST(ConfigMergeTest, FlagsOverrideFileValues) {
  RawConfig file;
  file.queue = "4";
  file.policy = "drop-oldest";
  RawConfig flags;
  flags.policy = "blocking";
  const RawConfig merged = merge_raw(file, flags);
  EXPECT_EQ(merged.queue, "4");
  EXPECT_EQ(merged.policy, "blocking");
}

TEST(ConfigMergeTest, EveryRawFieldIsMerged) {
  // Guards against a new RawConfig field missing from the key table.
  RawConfig flags;
  flags.fps = flags.queue = flags.duration = flags.frames = flags.warmup = flags.cost_ms =
      flags.cost_mode = flags.cost_wait = flags.policy = flags.resolution = flags.source =
          flags.input = flags.seed = flags.spike_ms = flags.spike_len = flags.spike_every =
              flags.target_ms = flags.exit_ratio = flags.window = flags.loop = flags.processor =
                  flags.kernel = "x";
  const RawConfig merged = merge_raw(RawConfig{}, flags);
  EXPECT_EQ(merged.fps, "x");
  EXPECT_EQ(merged.spike_every, "x");
  EXPECT_EQ(merged.input, "x");
  EXPECT_EQ(merged.window, "x");
  EXPECT_EQ(merged.loop, "x");
  EXPECT_EQ(merged.kernel, "x");
  EXPECT_EQ(run_key_paths().size(), 22u);
  EXPECT_EQ(sizeof(RawConfig), 22 * sizeof(std::optional<std::string>));
}

}  // namespace
}  // namespace frameflow
