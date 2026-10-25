#include "frameflow/policy/policy_factory.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>

namespace frameflow {
namespace {

FakeClock test_clock;  // the adaptive policy needs a clock; the others ignore it

TEST(PolicyFactoryTest, ListsAllFourPoliciesInDisplayOrder) {  // FR-POL-01
  EXPECT_EQ(policy_names(),
            (std::vector<std::string_view>{"blocking", "drop-oldest", "drop-newest", "adaptive"}));
}

TEST(PolicyFactoryTest, CreatesEachRegisteredPolicy) {
  for (std::string_view name : policy_names()) {
    const std::unique_ptr<OverloadPolicy> policy = make_policy(name, test_clock);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->name(), name);  // each name maps to its own class
  }
}

TEST(PolicyFactoryTest, CreatesIndependentInstances) {
  EXPECT_NE(make_policy("blocking", test_clock).get(), make_policy("blocking", test_clock).get());
}

TEST(PolicyFactoryTest, NamesAreCaseSensitive) {
  EXPECT_THROW(make_policy("Blocking", test_clock), std::invalid_argument);  // VR-07
  EXPECT_THROW(make_policy("DROP-OLDEST", test_clock), std::invalid_argument);
}

TEST(PolicyFactoryTest, UnknownNameErrorListsValidNames) {
  try {
    make_policy("leaky", test_clock);
    FAIL() << "expected std::invalid_argument";
  } catch (const std::invalid_argument& e) {
    EXPECT_EQ(std::string(e.what()),
              "unknown policy 'leaky' (valid: blocking, drop-oldest, drop-newest, adaptive)");
  }
}

TEST(PolicyFactoryTest, AdaptiveGetsItsParameters) {
  const std::unique_ptr<OverloadPolicy> policy =
      make_policy("adaptive", test_clock, AdaptiveParams{std::chrono::milliseconds(50), 0.5, 10});
  auto* adaptive = dynamic_cast<AdaptivePolicy*>(policy.get());
  ASSERT_NE(adaptive, nullptr);
  adaptive->on_delivered(std::chrono::milliseconds(51), Clock::duration::zero(),
                         test_clock.now());  // > T = 50 ms
  EXPECT_EQ(adaptive->state(), AdaptiveState::Shedding);
  EXPECT_THROW(
      make_policy("adaptive", test_clock, AdaptiveParams{std::chrono::milliseconds(50), 1.5, 10}),
      std::invalid_argument);
}

TEST(PolicyFactoryTest, EmptyNameIsRejected) {
  EXPECT_THROW(make_policy("", test_clock), std::invalid_argument);
}

}  // namespace
}  // namespace frameflow
