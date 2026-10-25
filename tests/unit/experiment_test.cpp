#include "frameflow/bench/experiment.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace frameflow {
namespace {

namespace fs = std::filesystem;

class ExperimentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("frameflow_exp_test_" + std::to_string(::getpid()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  ExperimentLoad load(const std::string& text,
                      const std::optional<std::string>& repeats = std::nullopt) {
    const fs::path path = dir_ / "e.yaml";
    std::ofstream(path, std::ios::binary) << text;
    return load_experiment(path, repeats);
  }

  fs::path dir_;
};

const std::string kBase =
    "experiment: e2\n"
    "source: { type: synthetic, resolution: 1280x720, fps: 30 }\n"
    "duration_s: 60\n";

TEST_F(ExperimentTest, CartesianProductInFileOrderLastKeyFastest) {
  const ExperimentLoad l =
      load(kBase + "matrix:\n  cost_ms: [40, 50, 67]\n  policy: [blocking, drop-oldest]\n");
  ASSERT_TRUE(l.ok()) << l.errors.front();
  const Experiment& e = l.experiment;
  EXPECT_EQ(e.name, "e2");
  EXPECT_EQ(e.repeats, 5u);  // VR-19 default
  ASSERT_EQ(e.configs.size(), 6u);
  const std::vector<std::string> names = {
      "cost_ms-40_policy-blocking", "cost_ms-40_policy-drop-oldest",
      "cost_ms-50_policy-blocking", "cost_ms-50_policy-drop-oldest",
      "cost_ms-67_policy-blocking", "cost_ms-67_policy-drop-oldest"};
  for (std::size_t i = 0; i < names.size(); ++i) EXPECT_EQ(e.configs[i].name, names[i]) << i;
  EXPECT_EQ(e.configs[2].config.cost_ms, 50.0);
  EXPECT_EQ(e.configs[2].config.policy, "blocking");
  EXPECT_EQ(e.configs[5].config.duration_s, 60.0);  // base values carried into every config
}

TEST_F(ExperimentTest, NamesAreStableAcrossLoads) {
  const std::string text = kBase + "matrix:\n  queue: [2, 4, 8]\n";
  const ExperimentLoad a = load(text);
  const ExperimentLoad b = load(text);
  ASSERT_TRUE(a.ok() && b.ok());
  ASSERT_EQ(a.experiment.configs.size(), b.experiment.configs.size());
  for (std::size_t i = 0; i < a.experiment.configs.size(); ++i) {
    EXPECT_EQ(a.experiment.configs[i].name, b.experiment.configs[i].name);
  }
}

TEST_F(ExperimentTest, NoMatrixGivesOneDefaultConfiguration) {
  const ExperimentLoad l = load(kBase);
  ASSERT_TRUE(l.ok());
  ASSERT_EQ(l.experiment.configs.size(), 1u);
  EXPECT_EQ(l.experiment.configs[0].name, "default");
}

TEST_F(ExperimentTest, NestedKeyInMatrixUsesLastPartInName) {
  const ExperimentLoad l = load(kBase + "matrix:\n  source.fps: [30, 60]\n");
  ASSERT_TRUE(l.ok()) << l.errors.front();
  EXPECT_EQ(l.experiment.configs[1].name, "fps-60");
  EXPECT_EQ(l.experiment.configs[1].config.fps, 60.0);
}

TEST_F(ExperimentTest, ConfigNameSanitisesValues) {
  EXPECT_EQ(config_name({{"spike_len", "500ms"}, {"policy", "a/b c"}}),
            "spike_len-500ms_policy-a-b-c");
  EXPECT_EQ(config_name({}), "default");
}

// VR-19: 1 to 100; --repeats overrides the file.
TEST_F(ExperimentTest, Repeats) {
  EXPECT_EQ(load(kBase + "repeats: 3\n").experiment.repeats, 3u);
  EXPECT_EQ(load(kBase + "repeats: 3\n", "7").experiment.repeats, 7u);
  EXPECT_TRUE(load(kBase + "repeats: 100\n").ok());
  for (const char* bad : {"0", "101", "two", "1.5"}) {
    const ExperimentLoad l = load(kBase + "repeats: " + bad + "\n");
    ASSERT_EQ(l.errors.size(), 1u) << bad;
    EXPECT_NE(l.errors[0].find("between 1 and 100"), std::string::npos) << l.errors[0];
  }
  EXPECT_FALSE(load(kBase, "0").ok());
}

TEST_F(ExperimentTest, InvalidValueNamesTheConfiguration) {
  const ExperimentLoad l = load(kBase + "matrix:\n  queue: [8, 0]\n");
  ASSERT_EQ(l.errors.size(), 1u);
  EXPECT_NE(l.errors[0].find("configuration queue-0: --queue 0 must be"), std::string::npos)
      << l.errors[0];
}

TEST_F(ExperimentTest, UnknownMatrixKeySuggestsClosest) {
  const ExperimentLoad l = load(kBase + "matrix:\n  polcy: [blocking]\n");
  ASSERT_EQ(l.errors.size(), 1u);
  EXPECT_NE(l.errors[0].find(":5:3: unknown matrix key 'polcy' (did you mean 'policy'?)"),
            std::string::npos)
      << l.errors[0];
}

TEST_F(ExperimentTest, MatrixValuesMustBeANonEmptyListWithoutDuplicates) {
  EXPECT_FALSE(load(kBase + "matrix:\n  queue: 8\n").ok());
  EXPECT_FALSE(load(kBase + "matrix:\n  queue: []\n").ok());
  EXPECT_FALSE(load(kBase + "matrix:\n  queue: [8, 8]\n").ok());
  EXPECT_FALSE(load(kBase + "matrix: [1, 2]\n").ok());
}

TEST_F(ExperimentTest, ExperimentNameIsRequiredAndSafe) {
  EXPECT_FALSE(load("duration_s: 60\nsource: {type: synthetic}\n").ok());
  EXPECT_FALSE(load("experiment: ../up\nsource: {type: synthetic}\n").ok());
  EXPECT_FALSE(load("experiment: a b\nsource: {type: synthetic}\n").ok());
}

TEST_F(ExperimentTest, UnknownRunKeyIsStillRejected) {
  EXPECT_FALSE(load(kBase + "polcy: blocking\n").ok());
}

// D45: the file's adaptive section reaches only the adaptive configurations.
TEST_F(ExperimentTest, AdaptiveSectionAppliesOnlyToAdaptiveConfigs) {
  const ExperimentLoad l = load(kBase +
                                "adaptive: { target_ms: 50 }\n"
                                "matrix:\n  policy: [blocking, adaptive]\n");
  ASSERT_TRUE(l.ok()) << l.errors.front();
  EXPECT_FALSE(l.experiment.configs[0].config.target_ms.has_value());
  EXPECT_EQ(l.experiment.configs[1].config.target_ms, 50.0);
}

TEST_F(ExperimentTest, AdaptiveMatrixKeyWithFixedPolicyIsAnError) {
  const ExperimentLoad l = load(kBase +
                                "matrix:\n  policy: [blocking, adaptive]\n"
                                "  adaptive.target_ms: [50, 100]\n");
  EXPECT_FALSE(l.ok());
}

TEST_F(ExperimentTest, TooManyConfigurationsAreRejected) {
  std::string values = "[";
  for (int i = 1; i <= 40; ++i) values += (i > 1 ? ", " : "") + std::to_string(i);
  values += "]";
  const ExperimentLoad l =
      load(kBase + "matrix:\n  queue: " + values + "\n  cost_ms: " + values + "\n");
  ASSERT_EQ(l.errors.size(), 1u);
  EXPECT_NE(l.errors[0].find("1600 configurations"), std::string::npos);
}

TEST_F(ExperimentTest, ExperimentJsonListsMatrixConfigsAndRunStatus) {
  const ExperimentLoad l = load(kBase + "matrix:\n  policy: [blocking, drop-oldest]\n");
  ASSERT_TRUE(l.ok());
  const std::vector<RunRecord> runs = {{"policy-blocking", 1, RunStatus::Completed, 0},
                                       {"policy-blocking", 2, RunStatus::Failed, 1},
                                       {"policy-drop-oldest", 1, RunStatus::Missing, 0}};
  const nlohmann::json j = make_experiment_json(l.experiment, runs, true);
  EXPECT_EQ(j["experiment"], "e2");
  EXPECT_EQ(j["schema_version"], 1);
  EXPECT_EQ(j["matrix"][0]["key"], "policy");
  EXPECT_EQ(j["matrix"][0]["values"], nlohmann::json::array({"blocking", "drop-oldest"}));
  EXPECT_EQ(j["configurations"][1]["name"], "policy-drop-oldest");
  EXPECT_EQ(j["configurations"][1]["params"]["policy"], "drop-oldest");
  EXPECT_EQ(j["runs"][0]["path"], "policy-blocking/rep_1");
  EXPECT_EQ(j["runs"][1]["status"], "failed");
  EXPECT_EQ(j["completed"], 1);
  EXPECT_EQ(j["failed"], nlohmann::json::array({"policy-blocking/rep_2"}));
  EXPECT_EQ(j["missing"], nlohmann::json::array({"policy-drop-oldest/rep_1"}));
  EXPECT_EQ(j["interrupted"], true);
  EXPECT_FALSE(j["runs"][2].contains("exit_code"));
}

// P3.5: every shipped experiment file loads, validates and expands as SPEC 11 describes.
struct ShippedExperiment {
  const char* file;
  const char* name;
  std::size_t configs;
};

class ExperimentFilesTest : public ::testing::TestWithParam<ShippedExperiment> {};

TEST_P(ExperimentFilesTest, LoadsAndExpands) {
  const ShippedExperiment& p = GetParam();
  const ExperimentLoad l = load_experiment(fs::path(FRAMEFLOW_SOURCE_DIR) / "experiments" / p.file);
  ASSERT_TRUE(l.ok()) << l.errors.front();
  EXPECT_EQ(l.experiment.name, p.name);
  EXPECT_EQ(l.experiment.repeats, 5u);
  EXPECT_EQ(l.experiment.configs.size(), p.configs);
  for (const ExperimentConfig& c : l.experiment.configs) {
    EXPECT_EQ(c.config.fps, 30.0) << c.name;
    EXPECT_EQ(c.config.duration_s, 60.0) << c.name;
    EXPECT_EQ(c.config.source, "synthetic") << c.name;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Shipped, ExperimentFilesTest,
    ::testing::Values(ShippedExperiment{"template.yaml", "my_experiment", 12},
                      ShippedExperiment{"e1_baseline.yaml", "e1_baseline", 4},
                      ShippedExperiment{"e2_sustained.yaml", "e2_sustained", 12},
                      ShippedExperiment{"e3_bursty.yaml", "e3_bursty", 3},
                      ShippedExperiment{"e4_capacity.yaml", "e4_capacity", 10},
                      ShippedExperiment{"e5_tuning.yaml", "e5_tuning", 9},
                      ShippedExperiment{"e6_real_load.yaml", "e6_real_load", 16}),
    [](const auto& info) {
      std::string name = info.param.name;
      name.erase(std::remove(name.begin(), name.end(), '_'), name.end());
      return name;
    });

TEST(ExperimentFilesTest, E3AndE5AreBurstyAsInSpec) {
  for (const char* file : {"e3_bursty.yaml", "e5_tuning.yaml"}) {
    const ExperimentLoad l = load_experiment(fs::path(FRAMEFLOW_SOURCE_DIR) / "experiments" / file);
    ASSERT_TRUE(l.ok()) << file;
    for (const ExperimentConfig& c : l.experiment.configs) {
      EXPECT_EQ(c.config.cost_mode, "bursty") << c.name;
      EXPECT_EQ(c.config.cost_ms, 20.0) << c.name;
      EXPECT_EQ(c.config.spike_ms, 80.0) << c.name;
      EXPECT_EQ(c.config.spike_len_s, 1.0) << c.name;
      EXPECT_EQ(c.config.spike_every_s, 5.0) << c.name;
    }
  }
  const ExperimentLoad e5 =
      load_experiment(fs::path(FRAMEFLOW_SOURCE_DIR) / "experiments" / "e5_tuning.yaml");
  ASSERT_TRUE(e5.ok());
  EXPECT_EQ(e5.experiment.configs[0].name, "target_ms-50_exit_ratio-0.5");
  EXPECT_EQ(e5.experiment.configs[8].config.target_ms, 200.0);
  EXPECT_EQ(e5.experiment.configs[8].config.exit_ratio, 0.9);
  EXPECT_EQ(e5.experiment.configs[8].config.window, 30u);
}

TEST(ExperimentFilesTest, E6UsesRealProcessorsAtBothResolutions) {
  const ExperimentLoad e6 =
      load_experiment(fs::path(FRAMEFLOW_SOURCE_DIR) / "experiments" / "e6_real_load.yaml");
  ASSERT_TRUE(e6.ok()) << e6.errors.front();
  EXPECT_EQ(e6.experiment.configs[0].name, "type-blur_resolution-1280x720_policy-blocking");
  EXPECT_EQ(e6.experiment.configs[0].config.processor, "blur");
  EXPECT_EQ(e6.experiment.configs[0].config.kernel, 15);
  EXPECT_EQ(e6.experiment.configs[15].config.processor, "canny");
  EXPECT_EQ(e6.experiment.configs[15].config.resolution, cv::Size(1920, 1080));
  EXPECT_EQ(e6.experiment.configs[15].config.policy, "adaptive");
}

// P5.9a: the measurement files load and expand (D56).
TEST(ExperimentFilesTest, PerformanceFilesLoad) {
  const fs::path perf = fs::path(FRAMEFLOW_SOURCE_DIR) / "experiments" / "perf";
  const std::vector<std::pair<const char*, std::size_t>> files = {
      {"pr01_pr02_baseline.yaml", 1}, {"pr09_throughput.yaml", 2}, {"pr07_pr08_memory.yaml", 4}};
  for (const auto& [file, configs] : files) {
    const ExperimentLoad l = load_experiment(perf / file);
    ASSERT_TRUE(l.ok()) << file << ": " << l.errors.front();
    EXPECT_EQ(l.experiment.configs.size(), configs) << file;
    EXPECT_EQ(l.experiment.repeats, 5u) << file;  // PR-11
  }
  const ExperimentLoad memory = load_experiment(perf / "pr07_pr08_memory.yaml");
  EXPECT_EQ(memory.experiment.configs[0].config.duration_s, 600.0);  // PR-08: 10 minutes
  const ExperimentLoad throughput = load_experiment(perf / "pr09_throughput.yaml");
  EXPECT_EQ(throughput.experiment.configs[0].config.fps, 240.0);
  EXPECT_EQ(throughput.experiment.configs[0].config.resolution, cv::Size(1280, 720));
}

TEST(ExperimentFilesTest, E2OverloadsAndE4CapacitiesMatchSpec) {
  const ExperimentLoad e2 =
      load_experiment(fs::path(FRAMEFLOW_SOURCE_DIR) / "experiments" / "e2_sustained.yaml");
  ASSERT_TRUE(e2.ok());
  EXPECT_EQ(e2.experiment.configs[0].config.cost_ms, 40.0);   // 1.2x
  EXPECT_EQ(e2.experiment.configs[11].config.cost_ms, 67.0);  // 2.0x
  EXPECT_EQ(e2.experiment.configs[11].config.policy, "adaptive");
  EXPECT_EQ(e2.experiment.configs[11].config.target_ms, 100.0);
  const ExperimentLoad e4 =
      load_experiment(fs::path(FRAMEFLOW_SOURCE_DIR) / "experiments" / "e4_capacity.yaml");
  ASSERT_TRUE(e4.ok());
  EXPECT_EQ(e4.experiment.configs.front().config.queue, 2u);
  EXPECT_EQ(e4.experiment.configs.back().config.queue, 32u);
  EXPECT_EQ(e4.experiment.configs.back().config.cost_ms, 50.0);  // 1.5x at 30 fps
}

}  // namespace
}  // namespace frameflow
