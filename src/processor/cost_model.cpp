#include "frameflow/processor/cost_model.hpp"

namespace frameflow {

CostModel::CostModel(Clock& clock, Clock::duration cost, CostWait wait)
    : clock_(clock), cost_(cost), wait_(wait) {}

void CostModel::apply() const {
  if (cost_ <= Clock::duration::zero()) return;
  const Clock::time_point end = clock_.now() + cost_;
  if (wait_ == CostWait::Sleep) {
    clock_.sleep_until(end);
    return;
  }
  while (clock_.now() < end) {
    // busy-wait: keeps the core busy for the whole cost, like real processing
  }
}

}  // namespace frameflow
