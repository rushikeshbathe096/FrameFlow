#include "frameflow/platform/signals.hpp"

#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <system_error>

#include "frameflow/exit_codes.hpp"

namespace frameflow {

namespace {

sigset_t stop_signals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  return set;
}

// Raw write(2): needs no stream, so it cannot interleave with or lock another thread's stream.
void write_stderr(const char* text, std::size_t size) {
  while (size > 0) {
    const ssize_t written = ::write(STDERR_FILENO, text, size);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return;
    text += written;
    size -= static_cast<std::size_t>(written);
  }
}

template <std::size_t N>
void write_stderr(const char (&text)[N]) {
  write_stderr(text, N - 1);
}

}  // namespace

SignalWatcher::SignalWatcher() {
  const sigset_t set = stop_signals();
  const int error = pthread_sigmask(SIG_BLOCK, &set, &old_mask_);
  if (error != 0) throw std::system_error(error, std::generic_category(), "pthread_sigmask");
  try {
    thread_ = std::thread([this] { watch(); });
  } catch (...) {
    pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr);
    throw;
  }
}

SignalWatcher::~SignalWatcher() {
  done_.store(true);
  thread_.join();  // wakes within one sigtimedwait timeout (50 ms)
  pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr);
}

void SignalWatcher::set_stop_handler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  handler_ = std::move(handler);
  // A signal racing with this call may run the handler twice (here and in on_signal); a stop
  // request is idempotent, and it can never be missed.
  if (count_.load() > 0 && handler_) handler_();
}

void SignalWatcher::clear_stop_handler() {
  std::lock_guard<std::mutex> lock(mutex_);
  handler_ = nullptr;
}

void SignalWatcher::watch() {
  const sigset_t set = stop_signals();
  const timespec timeout{0, 50'000'000};  // 50 ms: bounds how long the destructor waits
  while (!done_.load()) {
    if (sigtimedwait(&set, nullptr, &timeout) > 0) on_signal();
  }
}

void SignalWatcher::on_signal() {
  if (count_.fetch_add(1) > 0) {
    write_stderr("frameflow: error: forced exit, results may be incomplete\n");
    ::_exit(kExitForced);  // EC-11: immediate, no destructors, no file writes
  }
  write_stderr("frameflow: info: stopping... writing results\n");
  std::lock_guard<std::mutex> lock(mutex_);
  if (handler_) handler_();
}

}  // namespace frameflow
