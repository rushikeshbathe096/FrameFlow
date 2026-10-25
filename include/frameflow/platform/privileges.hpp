// privileges.hpp
// Responsibility: the root check (FR-ROLE-02, SR-AUTH-05). FrameFlow needs no privileges, so it
// refuses to run with effective UID 0 unless `--allow-root` is given, and then warns. The UID is
// a parameter so tests never need root; effective_uid() is the only call to geteuid() (RR-12).
// Thread-safety: pure functions; called on the main thread before any pipeline thread starts.
#pragma once

#include <optional>
#include <ostream>

namespace frameflow {

unsigned effective_uid();

// Prints the error (exit 5) or the --allow-root warning to err. Returns the exit code to stop
// with, or std::nullopt to continue.
std::optional<int> check_root(unsigned euid, bool allow_root, std::ostream& err);

}  // namespace frameflow
