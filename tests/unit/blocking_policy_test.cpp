#include "frameflow/policy/blocking_policy.hpp"

#include <gtest/gtest.h>

namespace frameflow {
namespace {

TEST(BlockingPolicyTest, InsertsWhileThereIsSpace) {
  BlockingPolicy policy;
  EXPECT_EQ(policy.on_push(QueueView{0, 2}, FrameMeta{}), PushDecision::Insert);
  EXPECT_EQ(policy.on_push(QueueView{1, 2}, FrameMeta{}), PushDecision::Insert);
}

TEST(BlockingPolicyTest, WaitsWhenFull) {
  BlockingPolicy policy;
  EXPECT_EQ(policy.on_push(QueueView{2, 2}, FrameMeta{}), PushDecision::Wait);
  EXPECT_EQ(policy.on_push(QueueView{1, 1}, FrameMeta{}), PushDecision::Wait);
}

TEST(BlockingPolicyTest, IsNamedBlocking) { EXPECT_EQ(BlockingPolicy().name(), "blocking"); }

}  // namespace
}  // namespace frameflow
