#include "frameflow/processor/cost_model.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace frameflow {
namespace {

using std::chrono::duration;
using std::chrono::milliseconds;
using Steady = std::chrono::steady_clock;

TEST(CostModelTest, ZeroCostReturnsImmediatelyAndLeavesFakeTimeUnchanged) {
  FakeClock clock;
  CostModel(clock, Clock::duration::zero(), CostWait::Busy).apply();  // must not spin
  CostModel(clock, Clock::duration::zero(), CostWait::Sleep).apply();
  EXPECT_EQ(clock.now(), Clock::time_point{});
}

TEST(CostModelTest, SleepModeAdvancesFakeClockByExactlyTheCost) {
  FakeClock clock;
  const CostModel cost(clock, milliseconds(50), CostWait::Sleep);
  cost.apply();
  EXPECT_EQ(clock.now(), Clock::time_point{} + milliseconds(50));
  cost.apply();
  EXPECT_EQ(clock.now(), Clock::time_point{} + milliseconds(100));
}

TEST(CostModelTest, BusyModeSpendsAtLeastTheCost) {
  SteadyClock clock;
  const auto start = Steady::now();
  CostModel(clock, milliseconds(5), CostWait::Busy).apply();
  EXPECT_GE(Steady::now() - start, milliseconds(5));  // lower bound only (D4)
}

TEST(CostModelTest, SleepModeSpendsAtLeastTheCost) {
  SteadyClock clock;
  const auto start = Steady::now();
  CostModel(clock, milliseconds(5), CostWait::Sleep).apply();
  EXPECT_GE(Steady::now() - start, milliseconds(5));
}

// PR-05: mean processing time within +/-5% of the configured cost for costs >= 5 ms.
// Runs only in the release preset (ctest label `timing`, D9) because sanitizers and -O0 distort
// timing.
double mean_cost_ms(CostWait wait, milliseconds cost, int repeats) {
  SteadyClock clock;
  const CostModel model(clock, cost, wait);
  const auto start = Steady::now();
  for (int i = 0; i < repeats; ++i) model.apply();
  return duration<double, std::milli>(Steady::now() - start).count() / repeats;
}

TEST(TimingCostModelTest, BusyMeanWithinFivePercent) {
  for (int cost_ms : {5, 20, 50}) {
    const double mean = mean_cost_ms(CostWait::Busy, milliseconds(cost_ms), 20);
    EXPECT_NEAR(mean, cost_ms, 0.05 * cost_ms) << "cost " << cost_ms << " ms";
  }
}

TEST(TimingCostModelTest, SleepMeanWithinFivePercent) {
  for (int cost_ms : {5, 20, 50}) {
    const double mean = mean_cost_ms(CostWait::Sleep, milliseconds(cost_ms), 20);
    EXPECT_NEAR(mean, cost_ms, 0.05 * cost_ms) << "cost " << cost_ms << " ms";
  }
}

}  // namespace
}  // namespace frameflow
