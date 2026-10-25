#include "frameflow/policy/policy_factory.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace frameflow {
namespace {

TEST(PolicyFactoryTest, ListsFixedPoliciesInDisplayOrder) {
  EXPECT_EQ(policy_names(),
            (std::vector<std::string_view>{"blocking", "drop-oldest", "drop-newest"}));
}

TEST(PolicyFactoryTest, CreatesEachRegisteredPolicy) {
  for (std::string_view name : policy_names()) {
    const std::unique_ptr<OverloadPolicy> policy = make_policy(name);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->name(), name);  // each name maps to its own class
  }
}

TEST(PolicyFactoryTest, CreatesIndependentInstances) {
  EXPECT_NE(make_policy("blocking").get(), make_policy("blocking").get());
}

TEST(PolicyFactoryTest, NamesAreCaseSensitive) {
  EXPECT_THROW(make_policy("Blocking"), std::invalid_argument);  // VR-07
  EXPECT_THROW(make_policy("DROP-OLDEST"), std::invalid_argument);
}

TEST(PolicyFactoryTest, UnknownNameErrorListsValidNames) {
  try {
    make_policy("leaky");
    FAIL() << "expected std::invalid_argument";
  } catch (const std::invalid_argument& e) {
    EXPECT_EQ(std::string(e.what()),
              "unknown policy 'leaky' (valid: blocking, drop-oldest, drop-newest)");
  }
}

TEST(PolicyFactoryTest, EmptyNameIsRejected) {
  EXPECT_THROW(make_policy(""), std::invalid_argument);
}

}  // namespace
}  // namespace frameflow
