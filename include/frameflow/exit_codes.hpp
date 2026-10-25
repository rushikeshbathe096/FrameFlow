// exit_codes.hpp
// Responsibility: the process exit codes of SRS 9.1, in one place.
// Thread-safety: constants only.
#pragma once

namespace frameflow {

// SRS 9.1 exit codes.
enum ExitCode : int {
  kExitOk = 0,
  kExitRuntime = 1,
  kExitUsage = 2,
  kExitInput = 3,
  kExitOutput = 4,
  kExitRoot = 5,
  kExitDisplay = 6,
  kExitForced = 130,
};

}  // namespace frameflow
