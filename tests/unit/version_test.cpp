#include "frameflow/version.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(VersionTest, MatchesProjectVersionFromCMake) {
  EXPECT_EQ(std::string(frameflow::version()), FRAMEFLOW_EXPECTED_VERSION);
}

TEST(VersionTest, IsNotEmpty) { EXPECT_FALSE(std::string(frameflow::version()).empty()); }
