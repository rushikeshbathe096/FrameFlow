#include "frameflow/run_command.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <opencv2/videoio.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace frameflow {
namespace {

namespace fs = std::filesystem;

class RunCommandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("frameflow_run_test_" + std::to_string(::getpid()));
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  void TearDown() override { fs::remove_all(root_); }

  int run(std::vector<const char*> args) {
    args.insert(args.begin(), "frameflow");
    out_.str("");
    err_.str("");
    return cli_main(static_cast<int>(args.size()), args.data(), out_, err_);
  }

  fs::path root_;
  std::ostringstream out_;
  std::ostringstream err_;
};

TEST_F(RunCommandTest, ShortSyntheticRunWritesAllFourFiles) {
  const std::string dir = (root_ / "run").string();
  const std::string out_flag = "--out=" + dir;
  const int code = run({"run", "--source=synthetic", "--fps=240", "--frames=48",
                        "--resolution=64x48", "--policy=drop-oldest", out_flag.c_str()});
  ASSERT_EQ(code, kExitOk) << err_.str();
  for (const char* file : {"frames.csv", "depth.csv", "events.csv", "summary.json"}) {
    EXPECT_TRUE(fs::is_regular_file(fs::path(dir) / file)) << file;
  }
  std::ifstream in(fs::path(dir) / "summary.json");
  const nlohmann::json summary = nlohmann::json::parse(in);
  EXPECT_EQ(summary["config"]["policy"], "drop-oldest");  // VR-24: effective config stored
  EXPECT_EQ(summary["config"]["frames"], 48);
  EXPECT_EQ(summary["config"]["resolution"], "64x48");
  EXPECT_EQ(summary["metrics"]["produced"], 48);
  EXPECT_EQ(summary["ended_by"], "frames");
  EXPECT_TRUE(out_.str().find("policy drop-oldest") != std::string::npos) << out_.str();
  EXPECT_TRUE(out_.str().find("summary.json") != std::string::npos);
  EXPECT_TRUE(out_.str().find("Result  ·  policy drop-oldest") != std::string::npos);  // S4
  // ER-07: the short-run warning is on stderr and in summary.json.
  EXPECT_TRUE(err_.str().find("frameflow: warning: run shorter than 10 s") != std::string::npos)
      << err_.str();
  EXPECT_EQ(summary["warnings"][0], "run shorter than 10 s; percentiles may be unstable");
}

TEST_F(RunCommandTest, InvalidOptionsExit2BeforeCreatingOutput) {
  const std::string dir = (root_ / "bad").string();
  const std::string out_flag = "--out=" + dir;
  const int code = run({"run", "--source=synthetic", "--fps=0", "--queue=0", out_flag.c_str()});
  EXPECT_EQ(code, kExitUsage);
  EXPECT_TRUE(err_.str().find("2 invalid options") != std::string::npos) << err_.str();
  EXPECT_FALSE(fs::exists(dir));
}

TEST_F(RunCommandTest, ExistingOutputFolderIsRefusedWithExit4) {
  const fs::path dir = root_ / "exists";
  fs::create_directory(dir);
  std::ofstream(dir / "keep.txt") << "mine";
  const std::string out_flag = "--out=" + dir.string();
  const int code = run({"run", "--source=synthetic", "--frames=10", "--fps=240", out_flag.c_str()});
  EXPECT_EQ(code, kExitOutput);
  EXPECT_TRUE(err_.str().find("output folder exists") != std::string::npos) << err_.str();
  EXPECT_TRUE(fs::exists(dir / "keep.txt"));
  EXPECT_FALSE(fs::exists(dir / "summary.json"));
}

TEST_F(RunCommandTest, MissingParentIsRefusedWithExit4) {
  const std::string out_flag = "--out=" + (root_ / "no" / "such" / "dir").string();
  const int code = run({"run", "--source=synthetic", "--frames=10", "--fps=240", out_flag.c_str()});
  EXPECT_EQ(code, kExitOutput);
  EXPECT_TRUE(err_.str().find("parent does not exist") != std::string::npos) << err_.str();
}

// FR-CLI-01, EC-19: --config values apply, and a flag beats the file in the effective config.
TEST_F(RunCommandTest, ConfigFileWithFlagOverride) {
  const fs::path config = root_ / "run.yaml";
  std::ofstream(config) << "source: { type: synthetic, resolution: 32x32, fps: 240 }\n"
                           "frames: 24\nqueue: 4\npolicy: drop-oldest\n";
  const std::string config_flag = "--config=" + config.string();
  const std::string out_flag = "--out=" + (root_ / "cfg").string();
  ASSERT_EQ(run({"run", config_flag.c_str(), "--policy=blocking", out_flag.c_str()}), kExitOk)
      << err_.str();
  std::ifstream in(root_ / "cfg" / "summary.json");
  const nlohmann::json summary = nlohmann::json::parse(in);
  EXPECT_EQ(summary["config"]["policy"], "blocking");  // flag wins
  EXPECT_EQ(summary["config"]["queue"], 4);            // file value kept
  EXPECT_EQ(summary["config"]["frames"], 24);
  EXPECT_NE(out_.str().find("policy blocking"), std::string::npos);  // S2 shows the flag value
}

TEST_F(RunCommandTest, BadConfigFileExits2) {
  const fs::path config = root_ / "bad.yaml";
  std::ofstream(config) << "polcy: blocking\n";
  const std::string config_flag = "--config=" + config.string();
  EXPECT_EQ(run({"run", config_flag.c_str()}), kExitUsage);
  EXPECT_NE(err_.str().find("frameflow: error: " + config.string() + ":1:1: unknown key 'polcy'"),
            std::string::npos)
      << err_.str();
}

// P5.1: a 10-frame 64x48 clip written by OpenCV.
fs::path write_clip(const fs::path& path) {
  cv::VideoWriter writer(path.string(), cv::CAP_OPENCV_MJPEG,
                         cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30.0, cv::Size(64, 48));
  for (int i = 0; i < 10; ++i) writer.write(cv::Mat(48, 64, CV_8UC3, cv::Scalar::all(20 * i)));
  return path;
}

TEST_F(RunCommandTest, FileSourceRunRecordsOnlyTheFileName) {
  const fs::path clip = write_clip(root_ / "clip.avi");
  const std::string input_flag = "--input=" + clip.string();
  const std::string out_flag = "--out=" + (root_ / "file").string();
  ASSERT_EQ(run({"run", input_flag.c_str(), "--frames=30", "--fps=240", out_flag.c_str()}), kExitOk)
      << err_.str();
  std::ifstream in(root_ / "file" / "summary.json");
  const nlohmann::json summary = nlohmann::json::parse(in);
  EXPECT_EQ(summary["config"]["source"], "file");
  EXPECT_EQ(summary["config"]["input"], "clip.avi");  // SR-08: no path outside --out
  EXPECT_EQ(summary["config"]["loop"], true);
  EXPECT_EQ(summary["metrics"]["produced"], 30);  // looped 3 times without gaps (EC-04)
  EXPECT_EQ(summary.dump().find(root_.string()), std::string::npos);
  EXPECT_NE(out_.str().find("source     file " + clip.string() + " 64x48 @ 240 fps"),
            std::string::npos)
      << out_.str();
}

// EC-05: --no-loop ends the run at the end of the file, with ended_by "input_eof".
TEST_F(RunCommandTest, NoLoopEndsAtEndOfInput) {
  const fs::path clip = write_clip(root_ / "clip.avi");
  const std::string input_flag = "--input=" + clip.string();
  const std::string out_flag = "--out=" + (root_ / "eof").string();
  ASSERT_EQ(
      run({"run", input_flag.c_str(), "--no-loop", "--duration=2", "--fps=60", out_flag.c_str()}),
      kExitOk)
      << err_.str();
  std::ifstream in(root_ / "eof" / "summary.json");
  const nlohmann::json summary = nlohmann::json::parse(in);
  EXPECT_EQ(summary["ended_by"], "input_eof");
  EXPECT_EQ(summary["metrics"]["produced"], 10);
  EXPECT_EQ(summary["config"]["loop"], false);
  EXPECT_EQ(summary["valid"], true);  // the (lowered, D21) warm-up passed
}

// VR-14, EC-06: a missing or undecodable input is exit 3, before any output is created.
TEST_F(RunCommandTest, BadInputExits3BeforeCreatingOutput) {
  const std::string out_flag = "--out=" + (root_ / "never").string();
  const std::string missing = "--input=" + (root_ / "missing.mp4").string();
  EXPECT_EQ(run({"run", missing.c_str(), out_flag.c_str()}), kExitInput);
  EXPECT_NE(err_.str().find("cannot open input"), std::string::npos) << err_.str();
  std::ofstream(root_ / "noise.mp4") << "definitely not a video";
  const std::string noise = "--input=" + (root_ / "noise.mp4").string();
  EXPECT_EQ(run({"run", noise.c_str(), out_flag.c_str()}), kExitInput);
  EXPECT_NE(err_.str().find("cannot decode input"), std::string::npos) << err_.str();
  EXPECT_FALSE(fs::exists(root_ / "never"));
}

// ER-03: decoding fails mid-run -> the run stops, partial results are written with
// valid=false, exit 1.
TEST_F(RunCommandTest, DecodeFailureMidRunExits1WithInvalidResults) {
  const fs::path full = root_ / "full.avi";
  {
    cv::VideoWriter writer(full.string(), cv::CAP_OPENCV_MJPEG,
                           cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30.0, cv::Size(64, 48));
    for (int i = 0; i < 30; ++i) writer.write(cv::Mat(48, 64, CV_8UC3, cv::Scalar::all(8 * i)));
  }
  std::ifstream in(full, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::ofstream(root_ / "truncated.avi", std::ios::binary)
      << bytes.substr(0, bytes.size() * 6 / 10);
  const std::string input_flag = "--input=" + (root_ / "truncated.avi").string();
  const std::string out_flag = "--out=" + (root_ / "partial").string();
  EXPECT_EQ(run({"run", input_flag.c_str(), "--frames=100", "--fps=240", out_flag.c_str()}),
            kExitRuntime);
  EXPECT_NE(err_.str().find("source: cannot decode input"), std::string::npos) << err_.str();
  EXPECT_NE(err_.str().find("partial results saved to"), std::string::npos);
  std::ifstream summary_in(root_ / "partial" / "summary.json");
  const nlohmann::json summary = nlohmann::json::parse(summary_in);
  EXPECT_EQ(summary["valid"], false);
  EXPECT_EQ(summary["ended_by"], "error");
  EXPECT_GT(summary["metrics"]["produced"].get<int>(), 0);
}

// ER-06: no display. Sets DISPLAY/WAYLAND_DISPLAY aside for the test and puts them back.
class NoDisplay {
 public:
  NoDisplay() {
    for (const char* name : {"DISPLAY", "WAYLAND_DISPLAY"}) {
      const char* value = std::getenv(name);
      saved_.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
      ::unsetenv(name);
    }
  }
  ~NoDisplay() {
    for (const auto& [name, value] : saved_) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
    }
  }
  NoDisplay(const NoDisplay&) = delete;
  NoDisplay& operator=(const NoDisplay&) = delete;

 private:
  std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

TEST_F(RunCommandTest, PreviewWithoutDisplayExits6BeforeAnything) {
  const NoDisplay no_display;
  const std::string out_flag = "--out=" + (root_ / "never").string();
  EXPECT_EQ(run({"run", "--source=synthetic", "--frames=10", "--preview", out_flag.c_str()}),
            kExitDisplay);
  EXPECT_NE(err_.str().find("--preview needs a display (hint: use --preview=auto"),
            std::string::npos)
      << err_.str();
  EXPECT_FALSE(fs::exists(root_ / "never"));
}

TEST_F(RunCommandTest, PreviewAutoWithoutDisplayRunsWithAWarning) {
  const NoDisplay no_display;
  const std::string out_flag = "--out=" + (root_ / "auto").string();
  EXPECT_EQ(run({"run", "--source=synthetic", "--frames=10", "--fps=240", "--resolution=32x32",
                 "--preview=auto", out_flag.c_str()}),
            kExitOk)
      << err_.str();
  EXPECT_NE(err_.str().find("frameflow: warning: no display"), std::string::npos) << err_.str();
  EXPECT_TRUE(fs::exists(root_ / "auto" / "summary.json"));
}

TEST_F(RunCommandTest, UnknownPreviewValueExits2) {
  EXPECT_EQ(run({"run", "--source=synthetic", "--preview=maybe"}), kExitUsage);
  EXPECT_NE(err_.str().find("--preview maybe must be on or auto"), std::string::npos) << err_.str();
}

// UIUX S3: --progress prints status lines on stderr during the run.
TEST_F(RunCommandTest, ProgressPrintsStatusLines) {
  const std::string out_flag = "--out=" + (root_ / "progress").string();
  ASSERT_EQ(run({"run", "--source=synthetic", "--duration=2", "--fps=60", "--resolution=32x32",
                 "--progress", out_flag.c_str()}),
            kExitOk)
      << err_.str();
  EXPECT_NE(err_.str().find("1/2 s  ·  out"), std::string::npos) << err_.str();
  EXPECT_NE(err_.str().find("queue "), std::string::npos);
}

// D56: a run long enough to be sampled records its memory use.
TEST_F(RunCommandTest, RecordsMemoryUse) {
  const std::string out_flag = "--out=" + (root_ / "memory").string();
  ASSERT_EQ(run({"run", "--source=synthetic", "--duration=2", "--fps=60", "--resolution=64x48",
                 out_flag.c_str()}),
            kExitOk)
      << err_.str();
  std::ifstream in(root_ / "memory" / "summary.json");
  const nlohmann::json memory = nlohmann::json::parse(in)["memory"];
  ASSERT_TRUE(memory["peak_rss_mb"].is_number());
  ASSERT_TRUE(memory["rss_after_warmup_mb"].is_number());  // warm-up 0 (D21): first tick at 1 s
  EXPECT_GT(memory["peak_rss_mb"].get<double>(), 0.0);
  EXPECT_GE(memory["peak_rss_mb"].get<double>(), memory["rss_end_mb"].get<double>());
  EXPECT_TRUE(memory["growth_pct"].is_number());
}

TEST_F(RunCommandTest, RootIsRefusedWithExit5BeforeAnythingElse) {
  RunArgs args;
  args.raw.source = "synthetic";
  args.out = (root_ / "as_root").string();
  EXPECT_EQ(run_command(args, 0, out_, err_), kExitRoot);
  EXPECT_TRUE(err_.str().find("refusing to run as root") != std::string::npos) << err_.str();
  EXPECT_FALSE(fs::exists(root_ / "as_root"));
}

TEST_F(RunCommandTest, AllowRootRunsWithWarning) {
  RunArgs args;
  args.raw.source = "synthetic";
  args.raw.frames = "10";
  args.raw.fps = "240";
  args.raw.resolution = "32x32";
  args.out = (root_ / "allowed").string();
  args.allow_root = true;
  EXPECT_EQ(run_command(args, 0, out_, err_), kExitOk) << err_.str();
  EXPECT_TRUE(err_.str().find("frameflow: warning: running as root") != std::string::npos);
  EXPECT_TRUE(fs::exists(root_ / "allowed" / "summary.json"));
}

TEST_F(RunCommandTest, OverwriteReplacesResultsAndKeepsOtherFiles) {
  const fs::path dir = root_ / "again";
  const std::string out_flag = "--out=" + dir.string();
  ASSERT_EQ(run({"run", "--source=synthetic", "--frames=10", "--fps=240", "--resolution=32x32",
                 out_flag.c_str()}),
            kExitOk)
      << err_.str();
  std::ofstream(dir / "notes.txt") << "mine";
  EXPECT_EQ(run({"run", "--source=synthetic", "--frames=20", "--fps=240", "--resolution=32x32",
                 out_flag.c_str()}),
            kExitOutput);
  ASSERT_EQ(run({"run", "--source=synthetic", "--frames=20", "--fps=240", "--resolution=32x32",
                 "--overwrite", out_flag.c_str()}),
            kExitOk)
      << err_.str();
  std::ifstream in(dir / "summary.json");
  EXPECT_EQ(nlohmann::json::parse(in)["config"]["frames"], 20);
  EXPECT_TRUE(fs::exists(dir / "notes.txt"));  // DR-11: only our own files are replaced
}

TEST_F(RunCommandTest, NothingIsWrittenOutsideOut) {
  const fs::path dir = root_ / "only";
  const std::string out_flag = "--out=" + dir.string();
  ASSERT_EQ(run({"run", "--source=synthetic", "--frames=10", "--fps=240", "--resolution=32x32",
                 out_flag.c_str()}),
            kExitOk);
  std::vector<fs::path> entries;
  for (const auto& entry : fs::recursive_directory_iterator(root_)) entries.push_back(entry.path());
  EXPECT_EQ(entries.size(), 5u);  // the folder and its four result files
}

}  // namespace
}  // namespace frameflow
