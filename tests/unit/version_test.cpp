#include "frameflow/version.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(VersionTest, MatchesProjectVersionFromCMake) {
  EXPECT_EQ(std::string(frameflow::version()), FRAMEFLOW_EXPECTED_VERSION);
}

TEST(VersionTest, IsNotEmpty) { EXPECT_FALSE(std::string(frameflow::version()).empty()); }

TEST(VersionTest, GitCommitIsHexOrUnknown) {
  const std::string commit = frameflow::git_commit();
  ASSERT_FALSE(commit.empty());
  if (commit != "unknown") {
    EXPECT_EQ(commit.find_first_not_of("0123456789abcdef"), std::string::npos) << commit;
  }
}
