#include "frameflow/policy/adaptive_policy.hpp"

#include <chrono>
#include <stdexcept>

namespace frameflow {

namespace {

const AdaptiveParams& checked(const AdaptiveParams& params) {
  if (params.target <= Clock::duration::zero()) {
    throw std::invalid_argument("AdaptivePolicy: target must be positive");
  }
  if (!(params.exit_ratio > 0.0 && params.exit_ratio < 1.0)) {
    throw std::invalid_argument("AdaptivePolicy: exit ratio must be in (0, 1)");
  }
  if (params.window == 0) throw std::invalid_argument("AdaptivePolicy: window must be >= 1");
  return params;
}

Clock::duration scaled(Clock::duration d, double factor) {
  return std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, Clock::duration::period>(d) * factor);
}

}  // namespace

AdaptivePolicy::AdaptivePolicy(Clock& clock, AdaptiveParams params)
    : clock_(clock),
      target_(checked(params).target),
      exit_threshold_(scaled(params.target, params.exit_ratio)),
      window_(params.window) {}

PushDecision AdaptivePolicy::on_push(const QueueView& queue, const FrameMeta& /*incoming*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == AdaptiveState::Shedding) return PushDecision::DropStale;
  return queue.full() ? PushDecision::Wait : PushDecision::Insert;  // NORMAL = blocking
}

bool AdaptivePolicy::is_stale(const FrameMeta& queued) const {
  const Clock::time_point now = clock_.now();
  std::lock_guard<std::mutex> lock(mutex_);  // queue mutex is held: order queue -> policy
  return stale_on_delivery(queued, now);
}

bool AdaptivePolicy::should_drop_on_pop(const FrameMeta& frame, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == AdaptiveState::Shedding && stale_on_delivery(frame, now);  // BR-07
}

void AdaptivePolicy::on_delivered(Clock::duration latency, Clock::duration processing,
                                  Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  const double processing_ns = std::chrono::duration<double, std::nano>(processing).count();
  est_cost_ns_ = kCostAlpha * processing_ns + (1.0 - kCostAlpha) * est_cost_ns_;  // D47
  window_.add(latency);
  const Clock::duration estimate = window_.p95().value_or(latency);  // never empty here
  AdaptiveState next = state_;
  if (state_ == AdaptiveState::Normal && estimate > target_) next = AdaptiveState::Shedding;
  if (state_ == AdaptiveState::Shedding && estimate < exit_threshold_) next = AdaptiveState::Normal;
  if (next == state_) return;  // inside the hysteresis band: no change (EC-13)
  changes_.push_back(StateChange{now, state_, next, estimate});
  state_ = next;
}

std::vector<StateChange> AdaptivePolicy::state_changes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return changes_;
}

AdaptiveState AdaptivePolicy::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::optional<Clock::duration> AdaptivePolicy::latency_estimate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return window_.p95();
}

Clock::duration AdaptivePolicy::estimated_cost() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::nano>(est_cost_ns_));
}

bool AdaptivePolicy::stale_on_delivery(const FrameMeta& frame, Clock::time_point now) const {
  if (!frame.t_cap) return false;
  const std::chrono::duration<double, std::nano> predicted_age =
      (now - *frame.t_cap) + std::chrono::duration<double, std::nano>(est_cost_ns_);
  return predicted_age > target_;
}

}  // namespace frameflow
