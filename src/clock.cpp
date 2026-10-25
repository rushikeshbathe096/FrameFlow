#include "frameflow/clock.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace frameflow {

Clock::time_point SteadyClock::now() const { return std::chrono::steady_clock::now(); }

void SteadyClock::sleep_until(time_point t) { std::this_thread::sleep_until(t); }

FakeClock::FakeClock(time_point start) : now_(start) {}

Clock::time_point FakeClock::now() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return now_;
}

void FakeClock::sleep_until(time_point t) {
  std::lock_guard<std::mutex> lock(mutex_);
  now_ = std::max(now_, t);
}

void FakeClock::advance(duration d) {
  if (d < duration::zero()) {
    throw std::invalid_argument("FakeClock::advance: negative duration");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  now_ += d;
}

}  // namespace frameflow
