#include "frameflow/metrics/json_reporter.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "frameflow/version.hpp"

namespace frameflow {
namespace {

using nlohmann::json;

Environment fixed_environment() {
  Environment env;
  env.cpu_model = "Test CPU";
  env.logical_cores = 8;
  env.total_ram_mb = 16000;
  env.kernel = "6.8.0-test";
  env.compiler = "gcc 13.2.0";
  env.opencv_version = "4.6.0";
  env.hostname_hash = hash_hostname("secret-host");
  return env;
}

RunReport delivered_report() {
  RunReport report;
  report.ended_by = EndedBy::Duration;
  report.valid = true;
  report.stats.produced = 100;
  report.stats.delivered = 98;
  report.stats.in_flight = 2;
  report.stats.window_s = 2.5;
  report.stats.p50_ms = 1.5;
  report.stats.p95_ms = 3.25;
  report.stats.p99_ms = 4.0;
  report.stats.max_ms = 5.0;
  report.stats.output_fps = 29.5;
  report.stats.depth_max = 3;
  return report;
}

TEST(JsonReporterTest, HasEveryTopLevelKey) {
  const json summary = make_summary_json(RunConfig{}, delivered_report(), fixed_environment(), {});
  for (const char* key : {"config", "environment", "metrics", "valid", "frameflow_version",
                          "git_commit", "schema_version", "warnings", "ended_by", "error"}) {
    EXPECT_TRUE(summary.contains(key)) << key;
  }
  EXPECT_EQ(summary["schema_version"], 1);
  EXPECT_EQ(summary["frameflow_version"], version());
  EXPECT_EQ(summary["git_commit"], git_commit());
  EXPECT_EQ(summary["valid"], true);
  EXPECT_EQ(summary["ended_by"], "duration");
  EXPECT_TRUE(summary["error"].is_null());
  EXPECT_EQ(summary["warnings"], json::array());
}

TEST(JsonReporterTest, EnvironmentHasEveryDr08Key) {
  const json env =
      make_summary_json(RunConfig{}, delivered_report(), fixed_environment(), {})["environment"];
  EXPECT_EQ(env["cpu_model"], "Test CPU");
  EXPECT_EQ(env["logical_cores"], 8);
  EXPECT_EQ(env["total_ram_mb"], 16000);
  EXPECT_EQ(env["kernel"], "6.8.0-test");
  EXPECT_EQ(env["compiler"], "gcc 13.2.0");
  EXPECT_EQ(env["opencv_version"], "4.6.0");
  EXPECT_EQ(env["hostname_hash"], hash_hostname("secret-host"));
  EXPECT_EQ(env.size(), 7u);
}

TEST(JsonReporterTest, PlainHostnameNeverAppears) {
  const std::string text =
      make_summary_json(RunConfig{}, delivered_report(), fixed_environment(), {}).dump();
  EXPECT_EQ(text.find("secret-host"), std::string::npos);
}

TEST(JsonReporterTest, ConfigRecordsEffectiveValuesAndSeed) {
  RunConfig config;
  config.fps = 60.0;
  config.queue = 4;
  config.duration_s.reset();
  config.frames = 500;
  config.policy = "drop-oldest";
  config.cost_wait = CostWait::Sleep;
  config.resolution = cv::Size(640, 480);
  config.source = "synthetic";
  config.seed = 7;
  const json c = make_summary_json(config, delivered_report(), fixed_environment(), {})["config"];
  EXPECT_EQ(c["fps"], 60.0);
  EXPECT_EQ(c["queue"], 4);
  EXPECT_TRUE(c["duration_s"].is_null());
  EXPECT_EQ(c["frames"], 500);
  EXPECT_EQ(c["warmup_s"], 3.0);
  EXPECT_EQ(c["cost_ms"], 0.0);
  EXPECT_EQ(c["cost_mode"], "fixed");
  EXPECT_EQ(c["cost_wait"], "sleep");
  EXPECT_EQ(c["policy"], "drop-oldest");
  EXPECT_EQ(c["resolution"], "640x480");
  EXPECT_EQ(c["source"], "synthetic");
  EXPECT_TRUE(c["input"].is_null());
  EXPECT_EQ(c["seed"], 7);
}

// D46: the spike schedule is reproducible from summary.json alone.
TEST(JsonReporterTest, BurstyConfigRecordsSpikeOffset) {
  RunConfig config;
  config.cost_mode = "bursty";
  config.spike_ms = 80.0;
  config.spike_len_s = 1.0;
  config.spike_every_s = 5.0;
  config.seed = 42;
  const json c = make_summary_json(config, delivered_report(), fixed_environment(), {})["config"];
  const double expected =
      std::chrono::duration<double>(first_spike_offset(std::chrono::seconds(5), 42)).count();
  EXPECT_DOUBLE_EQ(c["spike_offset_s"].get<double>(), expected);
  EXPECT_TRUE(make_summary_json(RunConfig{}, delivered_report(), fixed_environment(),
                                {})["config"]["spike_offset_s"]
                  .is_null());
}

TEST(JsonReporterTest, MetricsReuseRunStatsNames) {
  const json m =
      make_summary_json(RunConfig{}, delivered_report(), fixed_environment(), {})["metrics"];
  EXPECT_EQ(m["produced"], 100);
  EXPECT_EQ(m["delivered"], 98);
  EXPECT_EQ(m["dropped"], 0);
  EXPECT_EQ(m["in_flight"], 2);
  EXPECT_EQ(m["window_s"], 2.5);
  EXPECT_EQ(m["p50_ms"], 1.5);
  EXPECT_EQ(m["p95_ms"], 3.25);
  EXPECT_EQ(m["output_fps"], 29.5);
  EXPECT_EQ(m["depth_max"], 3);
  for (const char* key :
       {"window_produced", "window_delivered", "window_dropped_oldest", "window_dropped_newest",
        "window_dropped_stale", "p99_ms", "max_ms", "input_fps", "drop_rate_pct", "drop_oldest_pct",
        "drop_newest_pct", "drop_stale_pct", "depth_mean"}) {
    EXPECT_TRUE(m.contains(key)) << key;
  }
}

TEST(JsonReporterTest, AdaptiveFieldsAreNullForFixedPolicies) {
  const json summary = make_summary_json(RunConfig{}, delivered_report(), fixed_environment(), {});
  for (const char* key : {"target_ms", "exit_ratio", "window"}) {
    EXPECT_TRUE(summary["config"][key].is_null()) << key;
  }
  EXPECT_TRUE(summary["metrics"]["state_changes"].is_null());
  EXPECT_TRUE(summary["metrics"]["shedding_pct"].is_null());
}

TEST(JsonReporterTest, AdaptiveFieldsAreRecorded) {
  RunConfig config;
  config.policy = "adaptive";
  config.target_ms = 50.0;
  config.exit_ratio = 0.5;
  config.window = 10;
  RunReport report = delivered_report();
  report.stats.state_changes = 4;
  report.stats.shedding_pct = 25.0;
  const json summary = make_summary_json(config, report, fixed_environment(), {});
  EXPECT_EQ(summary["config"]["target_ms"], 50.0);
  EXPECT_EQ(summary["config"]["exit_ratio"], 0.5);
  EXPECT_EQ(summary["config"]["window"], 10);
  EXPECT_EQ(summary["metrics"]["state_changes"], 4);
  EXPECT_EQ(summary["metrics"]["shedding_pct"], 25.0);
}

TEST(JsonReporterTest, MemoryBlockWithNullsWhenNotSampled) {  // D56
  RunReport report = delivered_report();
  const json empty = make_summary_json(RunConfig{}, report, fixed_environment(), {})["memory"];
  for (const char* key : {"peak_rss_mb", "rss_after_warmup_mb", "rss_end_mb", "growth_pct"}) {
    EXPECT_TRUE(empty[key].is_null()) << key;
  }
  report.memory.peak_rss_mb = 120.5;
  report.memory.growth_pct = 1.25;
  const json memory = make_summary_json(RunConfig{}, report, fixed_environment(), {})["memory"];
  EXPECT_EQ(memory["peak_rss_mb"], 120.5);
  EXPECT_EQ(memory["growth_pct"], 1.25);
}

TEST(JsonReporterTest, LatencyIsNullWhenNothingDelivered) {
  RunReport report;  // all optional stats empty
  const json m = make_summary_json(RunConfig{}, report, fixed_environment(), {})["metrics"];
  for (const char* key : {"p50_ms", "p95_ms", "p99_ms", "max_ms", "output_fps", "depth_mean",
                          "depth_max", "drop_rate_pct"}) {
    EXPECT_TRUE(m[key].is_null()) << key;
  }
  EXPECT_EQ(m["delivered"], 0);  // counts stay numbers
}

TEST(JsonReporterTest, InvalidRunRecordsErrorAndWarnings) {
  RunReport report;
  report.ended_by = EndedBy::Error;
  report.error = "worker: boom";
  report.valid = false;
  const json summary =
      make_summary_json(RunConfig{}, report, fixed_environment(), {"late source", "short run"});
  EXPECT_EQ(summary["valid"], false);
  EXPECT_EQ(summary["ended_by"], "error");
  EXPECT_EQ(summary["error"], "worker: boom");
  EXPECT_EQ(summary["warnings"], json::array({"late source", "short run"}));
}

class JsonReporterFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("frameflow_json_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }
  std::filesystem::path dir_;
};

TEST_F(JsonReporterFileTest, WritesParseableFileEndingInNewline) {
  const json summary = make_summary_json(RunConfig{}, delivered_report(), fixed_environment(), {});
  write_summary_json(dir_, summary);

  std::ifstream in(dir_ / "summary.json", std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_FALSE(text.empty());
  EXPECT_EQ(text.back(), '\n');
  EXPECT_EQ(text.find('\r'), std::string::npos);
  EXPECT_EQ(json::parse(text), summary);
}

TEST_F(JsonReporterFileTest, InvalidUtf8InErrorDoesNotThrow) {
  RunReport report;
  report.error = "bad \xff byte";
  const json summary = make_summary_json(RunConfig{}, report, fixed_environment(), {});
  EXPECT_NO_THROW(write_summary_json(dir_, summary));
}

TEST_F(JsonReporterFileTest, ThrowsNamingFileWhenDirectoryMissing) {
  try {
    write_summary_json(dir_ / "missing", json::object());
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("summary.json"), std::string::npos);
  }
}

}  // namespace
}  // namespace frameflow
