// Spawns the real `frameflow` binary (posix_spawn, never system()) and sends it signals
// (FR-CLI-06, EC-08..EC-11, PR-06).
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The parent's environment, with TSan's 1 s exit pause turned off for the child: a forced exit
// (_exit while pipeline threads still run) would otherwise sleep there and fail the latency
// check. Harmless for builds without TSan.
std::vector<std::string> child_environment() {
  std::vector<std::string> env;
  bool has_tsan_options = false;
  for (char** var = environ; *var != nullptr; ++var) {
    env.emplace_back(*var);
    if (env.back().rfind("TSAN_OPTIONS=", 0) == 0) {
      env.back() += ":atexit_sleep_ms=0";
      has_tsan_options = true;
    }
  }
  if (!has_tsan_options) env.emplace_back("TSAN_OPTIONS=atexit_sleep_ms=0");
  return env;
}

std::vector<char*> as_argv(std::vector<std::string>& strings) {
  std::vector<char*> pointers;
  for (std::string& text : strings) pointers.push_back(text.data());
  pointers.push_back(nullptr);
  return pointers;
}

class SignalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("frameflow_signal_test_" + std::to_string(::getpid()));
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  void TearDown() override {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
    fs::remove_all(root_);
  }

  // Starts `frameflow run <args> --out=<root>/run` with stdout/stderr going to files.
  void spawn(std::vector<std::string> args) {
    args.insert(args.begin(), {FRAMEFLOW_BINARY, "run", "--source=synthetic", "--resolution=64x48",
                               "--out=" + out_dir().string()});
    std::vector<char*> argv = as_argv(args);
    std::vector<std::string> env_strings = child_environment();
    std::vector<char*> env = as_argv(env_strings);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, (root_ / "stdout").c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, (root_ / "stderr").c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_EQ(posix_spawn(&pid_, FRAMEFLOW_BINARY, &actions, nullptr, argv.data(), env.data()), 0);
    posix_spawn_file_actions_destroy(&actions);
  }

  bool wait_for_text(const char* file, const std::string& text, milliseconds timeout) {
    const auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
      if (read_text(root_ / file).find(text) != std::string::npos) return true;
      std::this_thread::sleep_for(milliseconds(5));
    }
    return false;
  }

  // Raw wait status, or nullopt if the child is still running after timeout.
  std::optional<int> wait_exit(milliseconds timeout) {
    const auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return status;
      }
      std::this_thread::sleep_for(milliseconds(2));
    }
    return std::nullopt;
  }

  void wait_until_running() {
    ASSERT_TRUE(wait_for_text("stdout", "run started", milliseconds(10000)))
        << read_text(root_ / "stderr");
  }

  nlohmann::json summary() const {
    std::ifstream in(out_dir() / "summary.json");
    return nlohmann::json::parse(in);
  }

  fs::path out_dir() const { return root_ / "run"; }
  std::string err_text() const { return read_text(root_ / "stderr"); }

  fs::path root_;
  pid_t pid_ = -1;
};

void expect_clean_exit_0(const std::optional<int>& status, const std::string& err) {
  ASSERT_TRUE(status.has_value()) << "still running; stderr:\n" << err;
  ASSERT_TRUE(WIFEXITED(*status)) << "killed by signal " << WTERMSIG(*status);
  EXPECT_EQ(WEXITSTATUS(*status), 0) << err;
}

TEST_F(SignalTest, SigintWhileSourceBlockedStopsGracefully) {
  // EC-09: blocking policy, 3x overload, queue of 2: the source is blocked on a full queue.
  spawn({"--fps=30", "--cost-ms=100", "--policy=blocking", "--queue=2", "--duration=60",
         "--warmup=0"});
  wait_until_running();
  std::this_thread::sleep_for(milliseconds(500));
  const auto sent = steady_clock::now();
  ASSERT_EQ(::kill(pid_, SIGINT), 0);
  const std::optional<int> status = wait_exit(milliseconds(5000));
  const auto latency = steady_clock::now() - sent;
  expect_clean_exit_0(status, err_text());
  EXPECT_LT(latency, milliseconds(1000));  // PR-06 (files here are tiny)
  EXPECT_NE(err_text().find("frameflow: info: stopping"), std::string::npos);
  const nlohmann::json s = summary();
  EXPECT_EQ(s["ended_by"], "stop_requested");
  EXPECT_EQ(s["valid"], true);
  EXPECT_GT(s["metrics"]["produced"].get<int>(), 0);
  for (const char* file : {"frames.csv", "depth.csv", "events.csv"}) {
    EXPECT_TRUE(fs::exists(out_dir() / file)) << file;
  }
}

TEST_F(SignalTest, SigtermWhileWorkerWaitsOnEmptyQueueStopsGracefully) {
  // EC-10: 2 fps and no cost: the worker spends almost all its time waiting for a frame.
  spawn({"--fps=2", "--duration=60", "--warmup=0"});
  wait_until_running();
  std::this_thread::sleep_for(milliseconds(700));
  const auto sent = steady_clock::now();
  ASSERT_EQ(::kill(pid_, SIGTERM), 0);
  const std::optional<int> status = wait_exit(milliseconds(5000));
  const auto latency = steady_clock::now() - sent;
  expect_clean_exit_0(status, err_text());
  EXPECT_LT(latency, milliseconds(1000));
  EXPECT_EQ(summary()["ended_by"], "stop_requested");
}

TEST_F(SignalTest, SigintDuringWarmupWritesInvalidResults) {
  // EC-08
  spawn({"--fps=30", "--duration=60", "--warmup=30"});
  wait_until_running();
  std::this_thread::sleep_for(milliseconds(300));
  ASSERT_EQ(::kill(pid_, SIGINT), 0);
  expect_clean_exit_0(wait_exit(milliseconds(5000)), err_text());
  EXPECT_EQ(summary()["valid"], false);
  EXPECT_EQ(summary()["ended_by"], "stop_requested");
}

TEST_F(SignalTest, SecondSigintExitsImmediatelyWith130) {
  // EC-11: a 3 s sleep cost keeps the graceful stop busy (the worker finishes its frame), so
  // the second signal arrives while the first is still being handled.
  spawn({"--fps=30", "--cost-ms=3000", "--cost-wait=sleep", "--duration=60", "--warmup=0"});
  wait_until_running();
  std::this_thread::sleep_for(milliseconds(200));
  ASSERT_EQ(::kill(pid_, SIGINT), 0);
  ASSERT_TRUE(wait_for_text("stderr", "stopping", milliseconds(5000))) << err_text();
  const auto sent = steady_clock::now();
  ASSERT_EQ(::kill(pid_, SIGINT), 0);
  const std::optional<int> status = wait_exit(milliseconds(5000));
  const auto latency = steady_clock::now() - sent;
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(WIFEXITED(*status)) << "killed by signal " << WTERMSIG(*status);
  EXPECT_EQ(WEXITSTATUS(*status), 130);
  EXPECT_LT(latency, milliseconds(1000));
  EXPECT_NE(err_text().find("frameflow: error: forced exit, results may be incomplete"),
            std::string::npos);
}

TEST_F(SignalTest, SigintThenSigtermAlsoForcesExit) {
  // Different signals never merge, so this checks "second signal" without timing luck.
  spawn({"--fps=30", "--cost-ms=3000", "--cost-wait=sleep", "--duration=60", "--warmup=0"});
  wait_until_running();
  ASSERT_EQ(::kill(pid_, SIGINT), 0);
  ASSERT_EQ(::kill(pid_, SIGTERM), 0);
  const std::optional<int> status = wait_exit(milliseconds(5000));
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(WIFEXITED(*status));
  EXPECT_EQ(WEXITSTATUS(*status), 130);
}

TEST_F(SignalTest, NormalRunExitsZeroWithoutSignals) {
  spawn({"--fps=240", "--frames=24"});
  expect_clean_exit_0(wait_exit(milliseconds(10000)), err_text());
  EXPECT_EQ(summary()["ended_by"], "frames");
  EXPECT_EQ(err_text().find("error"), std::string::npos) << err_text();  // warnings are allowed
}

}  // namespace
