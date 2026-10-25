// signals.hpp
// Responsibility: SIGINT/SIGTERM handling (FR-CLI-06). The constructor blocks both signals in the
// calling thread, so every thread started afterwards (source, worker) inherits the block, and
// starts one watcher thread that receives them with sigtimedwait. No async signal handler runs, so
// no code is restricted to async-signal-safe calls. The first signal calls the stop handler (a
// graceful stop: results are written, exit 0); the second prints a message and calls _exit(130)
// at once (D33). This is the only signal code in FrameFlow (RR-12).
// Thread-safety: set_stop_handler/clear_stop_handler/signal_count may be called from any thread;
// the handler runs on the watcher thread under the watcher's mutex, so it must not call back into
// the watcher. Construct it on the main thread before starting any other thread, and keep only
// one alive at a time.
#pragma once

#include <atomic>
#include <csignal>
#include <functional>
#include <mutex>
#include <thread>

namespace frameflow {

class SignalWatcher {
 public:
  SignalWatcher();
  ~SignalWatcher();  // stops and joins the watcher thread, restores the old signal mask

  SignalWatcher(const SignalWatcher&) = delete;
  SignalWatcher& operator=(const SignalWatcher&) = delete;

  // Called on the first signal. If that signal already came, handler is called immediately.
  void set_stop_handler(std::function<void()> handler);
  void clear_stop_handler();
  int signal_count() const { return count_.load(); }

 private:
  void watch();
  void on_signal();

  sigset_t old_mask_{};
  std::atomic<bool> done_{false};
  std::atomic<int> count_{0};
  std::mutex mutex_;  // guards handler_
  std::function<void()> handler_;
  std::thread thread_;  // last: starts after every other member is ready
};

}  // namespace frameflow
