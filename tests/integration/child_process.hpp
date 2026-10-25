// child_process.hpp (test helper)
// Responsibility: starts the real `frameflow` binary with posix_spawn (never system()), sends it
// signals, waits for text in its stdout/stderr files or for its exit; kills it if a test ends
// early. Used by the signal and bench interruption tests.
// Thread-safety: not thread-safe; one test thread owns each ChildProcess.
#pragma once

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
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace frameflow::testing {

inline std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The parent's environment, with TSan's 1 s exit pause turned off for the child: a forced exit
// (_exit while pipeline threads still run) would otherwise sleep there (D34). Harmless for builds
// without TSan.
inline std::vector<std::string> child_environment() {
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

inline std::vector<char*> as_argv(std::vector<std::string>& strings) {
  std::vector<char*> pointers;
  for (std::string& text : strings) pointers.push_back(text.data());
  pointers.push_back(nullptr);
  return pointers;
}

class ChildProcess {
 public:
  explicit ChildProcess(std::filesystem::path log_dir) : log_dir_(std::move(log_dir)) {}
  ~ChildProcess() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // args[0] is the program path; stdout/stderr go to <log_dir>/stdout and <log_dir>/stderr.
  bool start(std::vector<std::string> args) {
    std::vector<char*> argv = as_argv(args);
    std::vector<std::string> env_strings = child_environment();
    std::vector<char*> env = as_argv(env_strings);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, (log_dir_ / "stdout").c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, (log_dir_ / "stderr").c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int error = posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    return error == 0;
  }

  bool signal(int sig) const { return pid_ > 0 && ::kill(pid_, sig) == 0; }

  bool wait_for_text(const char* file, const std::string& text,
                     std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (read_text(log_dir_ / file).find(text) != std::string::npos) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  // Raw wait status, or nullopt if the child is still running after timeout.
  std::optional<int> wait_exit(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return status;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
  }

  std::string out_text() const { return read_text(log_dir_ / "stdout"); }
  std::string err_text() const { return read_text(log_dir_ / "stderr"); }

 private:
  std::filesystem::path log_dir_;
  pid_t pid_ = -1;
};

}  // namespace frameflow::testing
