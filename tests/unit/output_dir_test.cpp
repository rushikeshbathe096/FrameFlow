#include "frameflow/platform/output_dir.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace frameflow {
namespace {

namespace fs = std::filesystem;

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

unsigned mode_of(const fs::path& path) {
  struct stat info {};
  EXPECT_EQ(::stat(path.c_str(), &info), 0);
  return info.st_mode & 0777;
}

class OutputDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("frameflow_out_test_" + std::to_string(::getpid()));
    fs::remove_all(root_);
    fs::create_directories(root_ / "home");
    fs::create_directories(root_ / "outside");
    options_.allowed_roots = {root_ / "home"};
    old_umask_ = ::umask(022);  // process-wide; restored in TearDown (tests run sequentially)
  }
  void TearDown() override {
    ::umask(old_umask_);
    fs::permissions(root_, fs::perms::owner_all, fs::perm_options::add);
    std::error_code ec;
    fs::permissions(root_ / "locked", fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(root_);
  }

  std::optional<fs::path> prepare(const fs::path& out) {
    return prepare_out_dir(out, options_, err_);
  }

  fs::path root_;
  OutDirOptions options_;
  std::ostringstream err_;
  mode_t old_umask_ = 0;
};

TEST_F(OutputDirTest, NextRunDirSkipsExistingFolders) {
  EXPECT_EQ(next_run_dir(root_), root_ / "run_001");
  fs::create_directory(root_ / "run_001");
  fs::create_directory(root_ / "run_002");
  EXPECT_EQ(next_run_dir(root_), root_ / "run_003");
}

TEST_F(OutputDirTest, NextRunDirSkipsDanglingSymlink) {
  fs::create_symlink(root_ / "nowhere", root_ / "run_001");
  EXPECT_EQ(next_run_dir(root_), root_ / "run_002");
}

TEST_F(OutputDirTest, IsWithinComparesWholeComponents) {
  EXPECT_TRUE(is_within("/home/ana/results", "/home/ana"));
  EXPECT_TRUE(is_within("/home/ana", "/home/ana"));
  EXPECT_TRUE(is_within("/home/ana/x", "/home/ana/"));
  EXPECT_FALSE(is_within("/home/anabel/x", "/home/ana"));
  EXPECT_FALSE(is_within("/home", "/home/ana"));
  EXPECT_FALSE(is_within("/etc", "/home/ana"));
}

TEST_F(OutputDirTest, CreatesNewFolderWith0755AndReturnsAbsolutePath) {
  const auto dir = prepare(root_ / "home" / "run" / "");
  ASSERT_TRUE(dir.has_value()) << err_.str();
  EXPECT_EQ(*dir, root_ / "home" / "run");
  EXPECT_TRUE(dir->is_absolute());
  EXPECT_EQ(mode_of(*dir), 0755u);
}

TEST_F(OutputDirTest, ExistingFolderRefusedWithoutOverwrite) {
  fs::create_directory(root_ / "home" / "old");
  std::ofstream(root_ / "home" / "old" / "frames.csv") << "keep";
  EXPECT_FALSE(prepare(root_ / "home" / "old").has_value());
  EXPECT_NE(err_.str().find("output folder exists: " + (root_ / "home" / "old").string()),
            std::string::npos)
      << err_.str();
  EXPECT_NE(err_.str().find("--overwrite"), std::string::npos);
  EXPECT_EQ(read_text(root_ / "home" / "old" / "frames.csv"), "keep");
}

TEST_F(OutputDirTest, OverwriteRemovesOnlyResultFiles) {
  const fs::path old = root_ / "home" / "old";
  fs::create_directory(old);
  for (const char* name : kResultFiles) std::ofstream(old / name) << "old";
  std::ofstream(old / "notes.txt") << "mine";
  options_.overwrite = true;
  ASSERT_TRUE(prepare(old).has_value()) << err_.str();
  for (const char* name : kResultFiles) EXPECT_FALSE(fs::exists(old / name)) << name;
  EXPECT_EQ(read_text(old / "notes.txt"), "mine");
}

TEST_F(OutputDirTest, OverwriteUnlinksSymlinkedResultFileNotItsTarget) {
  const fs::path old = root_ / "home" / "old";
  fs::create_directory(old);
  std::ofstream(root_ / "outside" / "victim.txt") << "precious";
  fs::create_symlink(root_ / "outside" / "victim.txt", old / "summary.json");
  options_.overwrite = true;
  ASSERT_TRUE(prepare(old).has_value()) << err_.str();
  EXPECT_FALSE(fs::exists(fs::symlink_status(old / "summary.json")));
  EXPECT_EQ(read_text(root_ / "outside" / "victim.txt"), "precious");
}

TEST_F(OutputDirTest, ExistingFileIsNotAFolder) {
  std::ofstream(root_ / "home" / "file") << "x";
  options_.overwrite = true;
  EXPECT_FALSE(prepare(root_ / "home" / "file").has_value());
  EXPECT_NE(err_.str().find("is not a folder"), std::string::npos) << err_.str();
}

TEST_F(OutputDirTest, MissingParentIsRefused) {
  EXPECT_FALSE(prepare(root_ / "home" / "a" / "b").has_value());
  EXPECT_NE(err_.str().find("parent does not exist: " + (root_ / "home" / "a").string()),
            std::string::npos)
      << err_.str();
}

TEST_F(OutputDirTest, UnwritableParentNamesThePath) {
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  fs::create_directory(root_ / "locked");
  fs::permissions(root_ / "locked", fs::perms::owner_read | fs::perms::owner_exec);
  EXPECT_FALSE(prepare(root_ / "locked" / "run").has_value());
  EXPECT_NE(err_.str().find((root_ / "locked" / "run").string()), std::string::npos) << err_.str();
  EXPECT_NE(err_.str().find("Permission denied"), std::string::npos) << err_.str();
  EXPECT_NE(err_.str().find("check that you can write to " + (root_ / "locked").string()),
            std::string::npos);
}

TEST_F(OutputDirTest, SymlinkPointingOutsideAllowedRootsIsRefused) {
  fs::create_directory(root_ / "outside" / "target");
  fs::create_directory_symlink(root_ / "outside" / "target", root_ / "home" / "link");
  options_.overwrite = true;
  EXPECT_FALSE(prepare(root_ / "home" / "link").has_value());
  EXPECT_NE(err_.str().find("--allow-any-out"), std::string::npos) << err_.str();
}

TEST_F(OutputDirTest, SymlinkedParentPointingOutsideIsRefused) {
  fs::create_directory_symlink(root_ / "outside", root_ / "home" / "parent_link");
  EXPECT_FALSE(prepare(root_ / "home" / "parent_link" / "run").has_value());
  EXPECT_FALSE(fs::exists(root_ / "outside" / "run"));
}

TEST_F(OutputDirTest, AllowAnyOutAcceptsSymlinkOutside) {
  fs::create_directory_symlink(root_ / "outside", root_ / "home" / "parent_link");
  options_.allow_any_out = true;
  ASSERT_TRUE(prepare(root_ / "home" / "parent_link" / "run").has_value()) << err_.str();
  EXPECT_TRUE(fs::is_directory(root_ / "outside" / "run"));
}

TEST_F(OutputDirTest, SymlinkInsideAllowedRootIsAccepted) {
  fs::create_directory(root_ / "home" / "real");
  fs::create_directory_symlink(root_ / "home" / "real", root_ / "home" / "link");
  ASSERT_TRUE(prepare(root_ / "home" / "link" / "run").has_value()) << err_.str();
  EXPECT_TRUE(fs::is_directory(root_ / "home" / "real" / "run"));
}

TEST_F(OutputDirTest, RealFolderOutsideRootsIsFine) {
  // SR-05 restricts symlinks only; a plain path anywhere writable is the user's choice.
  ASSERT_TRUE(prepare(root_ / "outside" / "run").has_value()) << err_.str();
}

TEST_F(OutputDirTest, ResultFileIsCreated0644WithContent) {
  const fs::path path = root_ / "home" / "a.csv";
  write_result_file(path, [](std::ostream& out) { out << "x,y\n1,2\n"; });
  EXPECT_EQ(read_text(path), "x,y\n1,2\n");
  EXPECT_EQ(mode_of(path), 0644u);
}

TEST_F(OutputDirTest, LargeResultFileIsWrittenCompletely) {
  const fs::path path = root_ / "home" / "big.csv";
  const std::string line(99, 'a');
  write_result_file(path, [&](std::ostream& out) {
    for (int i = 0; i < 2000; ++i) out << line << '\n';  // 200 kB: several buffer flushes
  });
  EXPECT_EQ(fs::file_size(path), 200000u);
}

TEST_F(OutputDirTest, ExistingResultFileIsNeverOverwritten) {
  const fs::path path = root_ / "home" / "a.csv";
  std::ofstream(path) << "old";
  EXPECT_THROW(write_result_file(path, [](std::ostream& out) { out << "new"; }),
               std::runtime_error);
  EXPECT_EQ(read_text(path), "old");
}

TEST_F(OutputDirTest, SymlinkResultFileIsNeverFollowed) {
  std::ofstream(root_ / "outside" / "victim.txt") << "precious";
  fs::create_symlink(root_ / "outside" / "victim.txt", root_ / "home" / "a.csv");
  try {
    write_result_file(root_ / "home" / "a.csv", [](std::ostream& out) { out << "new"; });
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("a.csv"), std::string::npos) << e.what();
  }
  EXPECT_EQ(read_text(root_ / "outside" / "victim.txt"), "precious");
}

TEST_F(OutputDirTest, DiskFullNamesTheFileAndReason) {
  // D8: /dev/full accepts open() but every write fails with ENOSPC.
  const int fd = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  try {
    write_to_fd(fd, "results/run_001/frames.csv", [](std::ostream& out) { out << "data\n"; });
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("results/run_001/frames.csv"), std::string::npos);
    EXPECT_NE(std::string(e.what()).find("No space left on device"), std::string::npos) << e.what();
  }
  ::close(fd);
}

TEST_F(OutputDirTest, ThrowingBodyPropagatesAndLeavesNoOpenFile) {
  const fs::path path = root_ / "home" / "a.csv";
  EXPECT_THROW(write_result_file(path, [](std::ostream&) { throw std::runtime_error("boom"); }),
               std::runtime_error);
  EXPECT_TRUE(fs::exists(path));  // created, then the body failed; the fd was closed (ASan/lsan)
}

}  // namespace
}  // namespace frameflow
