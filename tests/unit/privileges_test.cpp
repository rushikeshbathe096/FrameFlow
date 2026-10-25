#include "frameflow/platform/privileges.hpp"

#include <gtest/gtest.h>

#include <sstream>

#include "frameflow/exit_codes.hpp"

namespace frameflow {
namespace {

TEST(PrivilegesTest, NormalUserContinuesSilently) {
  std::ostringstream err;
  EXPECT_EQ(check_root(1000, false, err), std::nullopt);
  EXPECT_EQ(err.str(), "");
}

TEST(PrivilegesTest, RootIsRefusedWithExit5AndReason) {
  std::ostringstream err;
  EXPECT_EQ(check_root(0, false, err), kExitRoot);
  EXPECT_EQ(err.str().rfind("frameflow: error: refusing to run as root", 0), 0u) << err.str();
  EXPECT_NE(err.str().find("--allow-root"), std::string::npos);
}

TEST(PrivilegesTest, AllowRootContinuesWithWarning) {
  std::ostringstream err;
  EXPECT_EQ(check_root(0, true, err), std::nullopt);
  EXPECT_EQ(err.str().rfind("frameflow: warning: running as root", 0), 0u) << err.str();
}

TEST(PrivilegesTest, AllowRootIsSilentForNormalUser) {
  std::ostringstream err;
  EXPECT_EQ(check_root(1000, true, err), std::nullopt);
  EXPECT_EQ(err.str(), "");
}

}  // namespace
}  // namespace frameflow
