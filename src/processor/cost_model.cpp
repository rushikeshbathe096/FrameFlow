#include "frameflow/processor/cost_model.hpp"

#include <random>
#include <stdexcept>

namespace frameflow {

Clock::duration first_spike_offset(Clock::duration every, std::uint64_t seed) {
  if (every <= Clock::duration::zero()) throw std::invalid_argument("spike period must be > 0");
  // std::mt19937's output sequence is fixed by the standard (distributions are not), so this is
  // reproducible across standard libraries. The 64-bit seed is folded into its 32-bit seed.
  std::mt19937 generator(static_cast<std::uint32_t>(seed ^ (seed >> 32)));
  const std::uint64_t high = generator();
  const std::uint64_t random = (high << 32) | generator();
  return Clock::duration(
      static_cast<Clock::duration::rep>(random % static_cast<std::uint64_t>(every.count())));
}

CostModel::CostModel(Clock& clock, Clock::duration cost, CostWait wait,
                     std::optional<BurstConfig> burst)
    : clock_(clock), cost_(cost), wait_(wait), burst_(burst) {
  if (burst_) {
    if (burst_->length <= Clock::duration::zero() || burst_->length >= burst_->every) {
      throw std::invalid_argument("CostModel: need 0 < spike length < spike period");
    }
    offset_ = first_spike_offset(burst_->every, burst_->seed);
  }
}

bool CostModel::in_spike(Clock::time_point t) const {
  if (!burst_) return false;
  const Clock::duration since_first = t - t0_ - offset_;
  if (since_first < Clock::duration::zero()) return false;  // before the first spike
  return since_first % burst_->every < burst_->length;
}

Clock::duration CostModel::cost_at(Clock::time_point t) const {
  if (burst_ && in_spike(t)) return burst_->spike_cost;
  return cost_;
}

void CostModel::apply() const {
  const Clock::time_point now = clock_.now();
  const Clock::duration cost = cost_at(now);
  if (cost <= Clock::duration::zero()) return;
  const Clock::time_point end = now + cost;
  if (wait_ == CostWait::Sleep) {
    clock_.sleep_until(end);
    return;
  }
  while (clock_.now() < end) {
    // busy-wait: keeps the core busy for the whole cost, like real processing
  }
}

}  // namespace frameflow
