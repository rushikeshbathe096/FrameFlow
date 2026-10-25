// cost_model.hpp
// Responsibility: synthetic per-frame processing cost. In fixed mode every frame costs the same
// duration (FR-PROC-02). In bursty mode a frame whose processing starts inside a spike costs the
// spike cost instead (FR-PROC-03). Spikes last `length` and repeat every `every`, starting at an
// offset in [0, every) derived only from the seed, measured from the run start, so the same
// settings always give the same schedule (FR-PROC-04, D5, D39). The cost is spent busy-waiting
// (default; burns CPU like a real filter) or sleeping (--cost-wait=sleep).
// Thread-safety: start() must be called before the worker thread starts (thread creation then
// orders it before every apply()); apply() and cost_at() are const and may be called from any
// thread. The Clock must be thread-safe (SteadyClock and FakeClock are).
// Note: busy-wait polls clock.now(), so with a FakeClock that nobody advances it would never
// finish. Use CostWait::Sleep with a FakeClock (FakeClock::sleep_until jumps instead of blocking).
#pragma once

#include <cstdint>
#include <optional>

#include "frameflow/clock.hpp"

namespace frameflow {

enum class CostWait { Busy, Sleep };

struct BurstConfig {
  Clock::duration spike_cost{0};
  Clock::duration length{0};  // 0 < length < every (VR-06)
  Clock::duration every{0};
  std::uint64_t seed = 42;
};

// Offset of the first spike from the run start, in [0, every): mt19937 seeded with `seed`, two
// 32-bit outputs combined, modulo `every` in clock ticks. Same seed, same offset on every platform.
Clock::duration first_spike_offset(Clock::duration every, std::uint64_t seed);

class CostModel {
 public:
  CostModel(Clock& clock, Clock::duration cost, CostWait wait,
            std::optional<BurstConfig> burst = std::nullopt);

  // Sets the run start that spike times are measured from (bursty mode only matters).
  void start(Clock::time_point t0) { t0_ = t0; }

  bool in_spike(Clock::time_point t) const;
  Clock::duration cost_at(Clock::time_point t) const;

  // Spends the cost of a frame whose processing starts now. Returns at once for a zero cost.
  void apply() const;

 private:
  Clock& clock_;
  Clock::duration cost_;
  CostWait wait_;
  std::optional<BurstConfig> burst_;
  Clock::duration offset_{0};
  Clock::time_point t0_{};
};

}  // namespace frameflow
