#include "frameflow/processor/cost_model.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>

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

// Bursty mode (FR-PROC-03, FR-PROC-04, D5, D39): 20 ms base, 80 ms spikes of 1 s every 5 s.
BurstConfig spikes(std::uint64_t seed) {
  return BurstConfig{milliseconds(80), std::chrono::seconds(1), std::chrono::seconds(5), seed};
}

TEST(CostModelBurstyTest, OffsetIsInsideOnePeriodAndSeedDetermined) {
  for (std::uint64_t seed : {0u, 1u, 42u, 123456789u}) {
    const Clock::duration offset = first_spike_offset(std::chrono::seconds(5), seed);
    EXPECT_GE(offset, Clock::duration::zero()) << seed;
    EXPECT_LT(offset, std::chrono::seconds(5)) << seed;
    EXPECT_EQ(offset, first_spike_offset(std::chrono::seconds(5), seed)) << seed;
  }
  EXPECT_NE(first_spike_offset(std::chrono::seconds(5), 1),
            first_spike_offset(std::chrono::seconds(5), 2));
}

TEST(CostModelBurstyTest, SpikesHaveConfiguredLengthAndPeriod) {
  FakeClock clock;
  CostModel model(clock, milliseconds(20), CostWait::Sleep, spikes(42));
  const Clock::time_point t0 = Clock::time_point{} + std::chrono::seconds(100);
  model.start(t0);
  const Clock::duration offset = first_spike_offset(std::chrono::seconds(5), 42);
  for (int n = 0; n < 4; ++n) {
    const Clock::time_point spike = t0 + offset + std::chrono::seconds(5 * n);
    EXPECT_EQ(model.cost_at(spike), milliseconds(80)) << "start of spike " << n;
    EXPECT_EQ(model.cost_at(spike + milliseconds(999)), milliseconds(80)) << n;
    EXPECT_EQ(model.cost_at(spike + std::chrono::seconds(1)), milliseconds(20)) << "end " << n;
    EXPECT_EQ(model.cost_at(spike + std::chrono::seconds(3)), milliseconds(20)) << n;
  }
  if (offset > Clock::duration::zero()) {
    EXPECT_EQ(model.cost_at(t0 + offset - Clock::duration(1)), milliseconds(20));  // before first
  }
}

TEST(CostModelBurstyTest, SpikeFractionMatchesLengthOverPeriod) {
  FakeClock clock;
  CostModel model(clock, milliseconds(20), CostWait::Sleep, spikes(7));
  model.start(Clock::time_point{});
  int in_spike = 0;
  const int samples = 60'000;  // every 1 ms of 60 s
  for (int ms = 0; ms < samples; ++ms) {
    if (model.in_spike(Clock::time_point{} + milliseconds(ms))) ++in_spike;
  }
  EXPECT_NEAR(static_cast<double>(in_spike) / samples, 0.2, 0.02);  // 1 s of every 5 s
}

TEST(CostModelBurstyTest, SameSeedGivesSameSchedule) {
  FakeClock clock;
  CostModel a(clock, milliseconds(20), CostWait::Sleep, spikes(99));
  CostModel b(clock, milliseconds(20), CostWait::Sleep, spikes(99));
  a.start(Clock::time_point{});
  b.start(Clock::time_point{});
  for (int ms = 0; ms < 20'000; ms += 7) {
    const Clock::time_point t = Clock::time_point{} + milliseconds(ms);
    ASSERT_EQ(a.cost_at(t), b.cost_at(t)) << ms;
  }
}

TEST(CostModelBurstyTest, ApplySpendsSpikeCostInsideSpike) {
  FakeClock clock;
  CostModel model(clock, milliseconds(20), CostWait::Sleep, spikes(42));
  model.start(clock.now());
  clock.advance(first_spike_offset(std::chrono::seconds(5), 42));  // first spike starts now
  const Clock::time_point before = clock.now();
  model.apply();
  EXPECT_EQ(clock.now() - before, milliseconds(80));
  clock.advance(std::chrono::seconds(2));  // well outside the spike
  const Clock::time_point later = clock.now();
  model.apply();
  EXPECT_EQ(clock.now() - later, milliseconds(20));
}

TEST(CostModelBurstyTest, FixedModeNeverSpikes) {
  FakeClock clock;
  const CostModel model(clock, milliseconds(20), CostWait::Sleep);
  EXPECT_FALSE(model.in_spike(clock.now()));
  EXPECT_EQ(model.cost_at(clock.now() + std::chrono::seconds(3)), milliseconds(20));
}

TEST(CostModelBurstyTest, RejectsInvalidSchedule) {
  FakeClock clock;
  BurstConfig bad = spikes(1);
  bad.length = bad.every;
  EXPECT_THROW(CostModel(clock, milliseconds(20), CostWait::Sleep, bad), std::invalid_argument);
  bad.length = Clock::duration::zero();
  EXPECT_THROW(CostModel(clock, milliseconds(20), CostWait::Sleep, bad), std::invalid_argument);
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
