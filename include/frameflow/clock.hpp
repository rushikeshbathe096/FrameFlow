// clock.hpp
// Responsibility: the only source of time in FrameFlow. Code that needs time takes a Clock&,
// so production uses SteadyClock and tests inject FakeClock for deterministic timing (AD-7).
// Time points are std::chrono::steady_clock::time_point, so wall-clock time cannot be mixed in
// by accident (EC-18).
// Thread-safety: SteadyClock and FakeClock are safe to use from any number of threads at once.
#pragma once

#include <chrono>
#include <mutex>

namespace frameflow {

class Clock {
 public:
  using duration = std::chrono::steady_clock::duration;
  using time_point = std::chrono::steady_clock::time_point;

  virtual ~Clock() = default;

  virtual time_point now() const = 0;

  // Returns once now() >= t. Returns immediately if t is already in the past.
  virtual void sleep_until(time_point t) = 0;
};

// Real time: std::chrono::steady_clock.
class SteadyClock final : public Clock {
 public:
  time_point now() const override;
  void sleep_until(time_point t) override;
};

// Test clock. Time only moves when advance() or sleep_until() is called.
// sleep_until(t) never blocks: it moves time to max(now, t) and returns, so a test cannot hang
// waiting for another thread to advance the clock (D14).
class FakeClock final : public Clock {
 public:
  explicit FakeClock(time_point start = time_point{});

  time_point now() const override;
  void sleep_until(time_point t) override;

  // Moves time forward by d. Throws std::invalid_argument if d is negative (a steady clock
  // never goes backwards).
  void advance(duration d);

 private:
  mutable std::mutex mutex_;  // mutable so that now() const can lock it
  time_point now_;
};

}  // namespace frameflow
