// adaptive_policy.hpp
// Responsibility: the latency-driven policy with hysteresis (SPEC 8.4). It keeps L, the p95 of
// the last W delivered latencies (FR-POL-05). NORMAL -> SHEDDING when L > T; SHEDDING -> NORMAL
// when L < T_low = exit_ratio x T (FR-POL-06); between the two thresholds the state never
// changes (EC-13). NORMAL behaves exactly like blocking and never drops (FR-POL-07, BR-07).
// SHEDDING, on push, drops queued frames that would be stale on delivery (`stale`) and then, if
// still full, the oldest; on pop, the worker drops such a frame (FR-POL-08). "Stale on delivery"
// means age + est_cost > T, where est_cost is an EWMA (alpha 0.2, starting at 0) of recent
// processing times, so a frame is not processed only to arrive late (D47). Every change is logged
// with time, old state, new state and L (FR-POL-09). Thread-safety: on_push()/is_stale() run on the
// source thread (queue lock held) while should_drop_on_pop()/on_delivered() run on the worker
// thread; `mutex_` guards the state, the window, the cost estimate and the change log. Lock order
// is always queue mutex -> policy mutex (the policy never calls the queue), so no deadlock is
// possible.
#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "frameflow/clock.hpp"
#include "frameflow/policy/latency_window.hpp"
#include "frameflow/policy/overload_policy.hpp"

namespace frameflow {

struct AdaptiveParams {
  Clock::duration target{std::chrono::milliseconds(100)};  // T, > 0 (VR-08)
  double exit_ratio = 0.7;                                 // T_low = exit_ratio x T, in (0, 1)
  std::size_t window = 30;                                 // W, >= 1 (VR-10 allows 5-1000)
};

class AdaptivePolicy final : public OverloadPolicy {
 public:
  AdaptivePolicy(Clock& clock, AdaptiveParams params);  // throws std::invalid_argument

  PushDecision on_push(const QueueView& queue, const FrameMeta& incoming) override;
  bool is_stale(const FrameMeta& queued) const override;
  bool should_drop_on_pop(const FrameMeta& frame, Clock::time_point now) override;
  void on_delivered(Clock::duration latency, Clock::duration processing,
                    Clock::time_point now) override;
  std::vector<StateChange> state_changes() const override;
  bool shedding() const override { return state() == AdaptiveState::Shedding; }
  std::string_view name() const override { return "adaptive"; }

  AdaptiveState state() const;
  std::optional<Clock::duration> latency_estimate() const;  // L; empty before any delivery
  Clock::duration estimated_cost() const;                   // EWMA of processing times (D47)

  static constexpr double kCostAlpha = 0.2;

 private:
  // Caller holds mutex_: true if the frame would be delivered after T (age + est_cost > T).
  bool stale_on_delivery(const FrameMeta& frame, Clock::time_point now) const;

  Clock& clock_;
  const Clock::duration target_;
  const Clock::duration exit_threshold_;  // T_low

  mutable std::mutex mutex_;  // guards the four members below
  AdaptiveState state_ = AdaptiveState::Normal;
  LatencyWindow window_;
  double est_cost_ns_ = 0.0;  // EWMA of processing time in nanoseconds; starts at 0 (D47)
  std::vector<StateChange> changes_;
};

}  // namespace frameflow
