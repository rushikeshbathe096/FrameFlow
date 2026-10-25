#include "frameflow/run_command.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
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
