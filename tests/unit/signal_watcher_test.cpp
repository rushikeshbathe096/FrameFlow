#include <gtest/gtest.h>
#include <pthread.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "frameflow/platform/signals.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;

bool blocked_in_this_thread(int signal) {
  sigset_t current;
  pthread_sigmask(SIG_BLOCK, nullptr, &current);
  return sigismember(&current, signal) == 1;
}

template <typename Predicate>
bool eventually(Predicate predicate) {
  for (int i = 0; i < 400; ++i) {
    if (predicate()) return true;
    std::this_thread::sleep_for(milliseconds(5));
  }
  return predicate();
}

TEST(SignalWatcherTest, BlocksSignalsWhileAliveAndRestoresAfter) {
  ASSERT_FALSE(blocked_in_this_thread(SIGINT));
  {
    SignalWatcher watcher;
    EXPECT_TRUE(blocked_in_this_thread(SIGINT));
    EXPECT_TRUE(blocked_in_this_thread(SIGTERM));
    bool child_blocked = false;
    std::thread([&] { child_blocked = blocked_in_this_thread(SIGINT); }).join();
    EXPECT_TRUE(child_blocked);  // threads started later inherit the block
  }
  EXPECT_FALSE(blocked_in_this_thread(SIGINT));
}

TEST(SignalWatcherTest, FirstSignalCallsStopHandler) {
  SignalWatcher watcher;
  std::atomic<int> calls{0};
  watcher.set_stop_handler([&] { ++calls; });
  ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);
  EXPECT_TRUE(eventually([&] { return calls.load() >= 1; }));
  EXPECT_EQ(watcher.signal_count(), 1);
}

TEST(SignalWatcherTest, SignalBeforeHandlerIsNotLost) {
  SignalWatcher watcher;
  ASSERT_EQ(::kill(::getpid(), SIGINT), 0);
  ASSERT_TRUE(eventually([&] { return watcher.signal_count() == 1; }));
  std::atomic<int> calls{0};
  watcher.set_stop_handler([&] { ++calls; });
  EXPECT_EQ(calls.load(), 1);  // called at once: the stop was already requested
}

TEST(SignalWatcherTest, ClearedHandlerIsNotCalled) {
  SignalWatcher watcher;
  std::atomic<int> calls{0};
  watcher.set_stop_handler([&] { ++calls; });
  watcher.clear_stop_handler();
  ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);
  ASSERT_TRUE(eventually([&] { return watcher.signal_count() == 1; }));
  EXPECT_EQ(calls.load(), 0);
}

TEST(SignalWatcherTest, DestructorReturnsPromptly) {
  const auto start = std::chrono::steady_clock::now();
  { SignalWatcher watcher; }
  EXPECT_LT(std::chrono::steady_clock::now() - start, milliseconds(500));
}

}  // namespace
}  // namespace frameflow
