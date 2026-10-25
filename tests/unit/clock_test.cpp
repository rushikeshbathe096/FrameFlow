#include "frameflow/clock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace frameflow {
namespace {

using std::chrono::milliseconds;
using TimePoint = Clock::time_point;

// Steady clock types only: this fails to compile if Clock ever switches to system_clock (EC-18).
static_assert(std::is_same_v<TimePoint::clock, std::chrono::steady_clock>);

TEST(SteadyClockTest, NowNeverDecreases) {
  SteadyClock clock;
  TimePoint previous = clock.now();
  for (int i = 0; i < 1000; ++i) {
    TimePoint current = clock.now();
    EXPECT_GE(current, previous);
    previous = current;
  }
}

TEST(SteadyClockTest, SleepUntilDoesNotReturnEarly) {
  SteadyClock clock;
  const TimePoint target = clock.now() + milliseconds(5);
  clock.sleep_until(target);
  EXPECT_GE(clock.now(), target);  // lower bound only: no upper bound on timing (D4)
}

TEST(SteadyClockTest, SleepUntilPastReturns) {
  SteadyClock clock;
  clock.sleep_until(clock.now() - milliseconds(100));  // must not hang
  SUCCEED();
}

TEST(FakeClockTest, DefaultStartsAtZero) {
  FakeClock clock;
  EXPECT_EQ(clock.now(), TimePoint{});
}

TEST(FakeClockTest, StartsAtGivenTime) {
  const TimePoint start = TimePoint{} + milliseconds(1234);
  FakeClock clock(start);
  EXPECT_EQ(clock.now(), start);
}

TEST(FakeClockTest, AdvanceMovesTimeExactly) {
  FakeClock clock;
  clock.advance(milliseconds(10));
  clock.advance(milliseconds(5));
  EXPECT_EQ(clock.now(), TimePoint{} + milliseconds(15));
}

TEST(FakeClockTest, AdvanceZeroIsAllowed) {
  FakeClock clock;
  clock.advance(Clock::duration::zero());
  EXPECT_EQ(clock.now(), TimePoint{});
}

TEST(FakeClockTest, NegativeAdvanceThrowsAndLeavesTimeUnchanged) {
  FakeClock clock(TimePoint{} + milliseconds(50));
  EXPECT_THROW(clock.advance(milliseconds(-1)), std::invalid_argument);
  EXPECT_EQ(clock.now(), TimePoint{} + milliseconds(50));
}

TEST(FakeClockTest, SleepUntilFutureJumpsToTarget) {
  FakeClock clock;
  const TimePoint target = TimePoint{} + milliseconds(33);
  clock.sleep_until(target);
  EXPECT_EQ(clock.now(), target);
}

TEST(FakeClockTest, SleepUntilPastLeavesTimeUnchanged) {
  FakeClock clock(TimePoint{} + milliseconds(100));
  clock.sleep_until(TimePoint{} + milliseconds(40));
  EXPECT_EQ(clock.now(), TimePoint{} + milliseconds(100));
}

TEST(FakeClockTest, WorksThroughClockInterface) {
  FakeClock fake;
  Clock& clock = fake;  // how production code will see it
  clock.sleep_until(TimePoint{} + milliseconds(7));
  EXPECT_EQ(clock.now(), TimePoint{} + milliseconds(7));
}

// Runs under TSan in the tsan preset: one writer, one reader, no data race, time never decreases.
TEST(FakeClockTest, ConcurrentAdvanceAndReadIsSafeAndMonotonic) {
  FakeClock clock;
  constexpr int kSteps = 10000;
  std::atomic<bool> saw_decrease{false};

  std::thread reader([&] {
    TimePoint previous = clock.now();
    while (clock.now() < TimePoint{} + milliseconds(kSteps)) {
      const TimePoint current = clock.now();
      if (current < previous) saw_decrease = true;
      previous = current;
    }
  });
  for (int i = 0; i < kSteps; ++i) clock.advance(milliseconds(1));
  reader.join();

  EXPECT_FALSE(saw_decrease);
  EXPECT_EQ(clock.now(), TimePoint{} + milliseconds(kSteps));
}

}  // namespace
}  // namespace frameflow
