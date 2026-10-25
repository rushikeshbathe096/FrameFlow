#include "frameflow/platform/privileges.hpp"

#include <unistd.h>

#include "frameflow/exit_codes.hpp"

namespace frameflow {

unsigned effective_uid() { return static_cast<unsigned>(::geteuid()); }

std::optional<int> check_root(unsigned euid, bool allow_root, std::ostream& err) {
  if (euid != 0) return std::nullopt;
  if (!allow_root) {
    err << "frameflow: error: refusing to run as root: FrameFlow needs no privileges and as root "
           "a wrong --out could overwrite system files (hint: run as a normal user, or add "
           "--allow-root)\n";
    return kExitRoot;
  }
  err << "frameflow: warning: running as root (--allow-root): output files will be owned by "
         "root and a wrong --out could overwrite system files\n";
  return std::nullopt;
}

}  // namespace frameflow
