#include "frameflow/version.hpp"

namespace frameflow {

const char* version() { return kVersionString; }

const char* git_commit() { return kGitCommit; }

}  // namespace frameflow
