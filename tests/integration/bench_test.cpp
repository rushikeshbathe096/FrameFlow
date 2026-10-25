// `frameflow bench` (FR-CLI-02, VR-19, DR-05, BR-11, EC-20): layout of a tiny matrix, the
// median table, and an interrupted bench that keeps finished runs and lists the missing ones.
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "frameflow/bench/bench_runner.hpp"
#include "frameflow/cli.hpp"

namespace frameflow {
namespace {

namespace fs = std::filesystem;
using std::chrono::milliseconds;

// 2 configurations x 2 repeats x 1 s.
const char* const kTiny =
    "experiment: tiny\n"
    "source: { type: synthetic, resolution: 32x32, fps: 50 }\n"
    "duration_s: 1\n"
    "warmup_s: 0.2\n"
    "cost_ms: 30\n"
    "repeats: 2\n"
    "matrix:\n"
    "  policy: [blocking, drop-oldest]\n";

nlohmann::json read_json(const fs::path& path) {
  std::ifstream in(path);
  return nlohmann::json::parse(in);
}

class BenchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  void TearDown() override { fs::remove_all(root_); }

  fs::path write_experiment(const std::string& text) {
    const fs::path path = root_ / "e.yaml";
    std::ofstream(path) << text;
    return path;
  }

  int bench(std::vector<std::string> extra) {
    std::vector<std::string> args = {
        "frameflow", "bench", "--config=" + (root_ / "e.yaml").string(), "--out=" + root_.string()};
    args.insert(args.end(), extra.begin(), extra.end());
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    out_.str("");
    err_.str("");
    return cli_main(static_cast<int>(argv.size()), argv.data(), out_, err_);
  }

  fs::path root_ =
      fs::temp_directory_path() / ("frameflow_bench_test_" + std::to_string(::getpid()));
  std::ostringstream out_;
  std::ostringstream err_;
};

TEST_F(BenchTest, TinyMatrixLayoutAndExperimentJson) {
  write_experiment(kTiny);
  ASSERT_EQ(bench({}), kExitOk) << err_.str();
  const fs::path exp = root_ / "tiny";
  for (const char* config : {"policy-blocking", "policy-drop-oldest"}) {
    for (const char* rep : {"rep_1", "rep_2"}) {
      for (const char* file : {"frames.csv", "depth.csv", "events.csv", "summary.json"}) {
        EXPECT_TRUE(fs::is_regular_file(exp / config / rep / file)) << config << "/" << rep;
      }
    }
  }
  EXPECT_EQ(read_json(exp / "policy-drop-oldest" / "rep_2" / "summary.json")["config"]["policy"],
            "drop-oldest");
  const nlohmann::json j = read_json(exp / "experiment.json");
  EXPECT_EQ(j["completed"], 4);
  EXPECT_EQ(j["missing"], nlohmann::json::array());
  EXPECT_EQ(j["interrupted"], false);
  EXPECT_EQ(j["runs"].size(), 4u);
  EXPECT_FALSE(fs::exists(exp / ".experiment.json.tmp"));
  // S5: progress lines and the median table (BR-11).
  const std::string text = out_.str();
  EXPECT_NE(text.find("Experiment tiny  ·  2 configurations x 2 repeats = 4 runs"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("[4/4]  policy=drop-oldest rep=2"), std::string::npos) << text;
  EXPECT_NE(text.find("Median over completed repeats"), std::string::npos);
  EXPECT_NE(text.find("policy-blocking"), std::string::npos);
}

TEST_F(BenchTest, RepeatsFlagBeatsTheFile) {
  write_experiment(std::string(kTiny) + "");
  ASSERT_EQ(bench({"--repeats=1"}), kExitOk) << err_.str();
  EXPECT_EQ(read_json(root_ / "tiny" / "experiment.json")["completed"], 2);
  EXPECT_FALSE(fs::exists(root_ / "tiny" / "policy-blocking" / "rep_2"));
}

TEST_F(BenchTest, InvalidExperimentExits2) {
  write_experiment("experiment: x\nsource: {type: synthetic}\nmatrix:\n  queue: [0]\n");
  EXPECT_EQ(bench({}), kExitUsage);
  EXPECT_NE(err_.str().find("configuration queue-0: --queue 0"), std::string::npos) << err_.str();
  EXPECT_FALSE(fs::exists(root_ / "x"));
}

TEST_F(BenchTest, BadRepeatsFlagExits2) {
  write_experiment(kTiny);
  EXPECT_EQ(bench({"--repeats=0"}), kExitUsage);
}

TEST_F(BenchTest, ExistingExperimentFolderNeedsOverwrite) {
  write_experiment(std::string(kTiny));
  ASSERT_EQ(bench({"--repeats=1"}), kExitOk) << err_.str();
  EXPECT_EQ(bench({"--repeats=1"}), kExitOutput);
  EXPECT_NE(err_.str().find("output folder exists"), std::string::npos);
  EXPECT_EQ(bench({"--repeats=1", "--overwrite"}), kExitOk) << err_.str();
}

TEST_F(BenchTest, MissingConfigFlagIsUsageError) {
  const char* argv[] = {"frameflow", "bench"};
  EXPECT_EQ(cli_main(2, argv, out_, err_), kExitUsage);
}

// EC-20: Ctrl+C in the middle keeps completed repetitions and lists the missing ones.
TEST_F(BenchTest, InterruptedBenchKeepsCompletedRunsAndListsMissing) {
  write_experiment(
      "experiment: stopme\n"
      "source: { type: synthetic, resolution: 32x32, fps: 50 }\n"
      "duration_s: 3\nwarmup_s: 0\nrepeats: 2\n"
      "matrix:\n  policy: [blocking, drop-oldest]\n");
  testing::ChildProcess child(root_);
  ASSERT_TRUE(child.start({FRAMEFLOW_BINARY, "bench", "--config=" + (root_ / "e.yaml").string(),
                           "--out=" + root_.string()}));
  ASSERT_TRUE(child.wait_for_text("stdout", "[2/4]", milliseconds(20000))) << child.err_text();
  ASSERT_TRUE(child.signal(SIGINT));  // during the second run
  const std::optional<int> status = child.wait_exit(milliseconds(10000));
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(WIFEXITED(*status));
  EXPECT_EQ(WEXITSTATUS(*status), 0) << child.err_text();

  const nlohmann::json j = read_json(root_ / "stopme" / "experiment.json");
  EXPECT_EQ(j["interrupted"], true);
  EXPECT_EQ(j["completed"], 1);
  EXPECT_EQ(j["runs"][0]["status"], "completed");
  EXPECT_EQ(j["runs"][1]["status"], "interrupted");
  EXPECT_EQ(j["missing"],
            nlohmann::json::array(
                {"policy-blocking/rep_2", "policy-drop-oldest/rep_1", "policy-drop-oldest/rep_2"}));
  EXPECT_TRUE(fs::exists(root_ / "stopme" / "policy-blocking" / "rep_1" / "summary.json"));
  EXPECT_FALSE(fs::exists(root_ / "stopme" / "policy-drop-oldest"));  // never started
  EXPECT_NE(child.err_text().find("bench interrupted; 3 of 4 runs missing"), std::string::npos)
      << child.err_text();
  EXPECT_NE(child.err_text().find("  policy-drop-oldest/rep_2\n"), std::string::npos);
}

TEST(BenchMedianTest, MedianOddEvenAndEmpty) {
  EXPECT_EQ(median({3.0, 1.0, 2.0}), 2.0);
  EXPECT_EQ(median({4.0, 1.0, 3.0, 2.0}), 2.5);
  EXPECT_EQ(median({}), std::nullopt);
}

TEST(BenchMedianTest, TableUsesOnlyCompletedRepeats) {
  Experiment e;
  e.name = "x";
  e.configs.push_back({"policy-blocking", {{"policy", "blocking"}}, RunConfig{}});
  std::vector<RepResult> reps(3);
  for (std::size_t i = 0; i < reps.size(); ++i) {
    reps[i].config = "policy-blocking";
    reps[i].rep = i + 1;
    reps[i].status = RunStatus::Completed;
    reps[i].p95_ms = 10.0 * static_cast<double>(i + 1);  // 10, 20, 30
  }
  reps[2].status = RunStatus::Interrupted;  // its 30 ms must not count
  const std::string table = format_median_table(e, reps);
  EXPECT_NE(table.find("policy-blocking  2     —        15.0 [10.0-20.0]"), std::string::npos)
      << table;
}

}  // namespace
}  // namespace frameflow
