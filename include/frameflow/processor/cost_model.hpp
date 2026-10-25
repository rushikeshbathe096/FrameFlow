// cost_model.hpp
// Responsibility: synthetic per-frame processing cost (FR-PROC-02). In fixed mode every frame
// costs the same duration, spent either busy-waiting (default; burns CPU like a real filter) or
// sleeping (--cost-wait=sleep).
// Thread-safety: apply() is const and may be called from any thread; the Clock it uses must be
// thread-safe (SteadyClock and FakeClock are).
// Note: busy-wait polls clock.now(), so with a FakeClock that nobody advances it would never
// finish. Use CostWait::Sleep with a FakeClock (FakeClock::sleep_until jumps instead of blocking).
#pragma once

#include "frameflow/clock.hpp"

namespace frameflow {

enum class CostWait { Busy, Sleep };

class CostModel {
 public:
  CostModel(Clock& clock, Clock::duration cost, CostWait wait);

  // Spends the configured cost for one frame. Returns at once if the cost is zero or negative.
  void apply() const;

 private:
  Clock& clock_;
  Clock::duration cost_;
  CostWait wait_;
};

}  // namespace frameflow
