// output_dir.hpp
// Responsibility: output-folder safety (BR-12, VR-17, SR-05, SR-06, SR-AUTH-04, DR-11). Resolves
// `--out` to an absolute path, refuses an existing folder unless `--overwrite`, refuses a path
// that resolves through a symlink to outside the user's home and the current folder unless
// `--allow-any-out` (D31), creates the folder (0755 before umask), and creates each result file
// with O_EXCL | O_NOFOLLOW (0644 before umask) so an existing file or symlink is never followed
// or overwritten. With
// `--overwrite` only FrameFlow's own result files in the folder are removed first (D30).
// Thread-safety: free functions with no shared state; called on the main thread, before the
// pipeline threads start or after they have joined (FR-MET-07).
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <vector>

namespace frameflow {

inline constexpr std::array<const char*, 4> kResultFiles = {"frames.csv", "depth.csv", "events.csv",
                                                            "summary.json"};

struct OutDirOptions {
  bool overwrite = false;                            // --overwrite
  bool allow_any_out = false;                        // --allow-any-out
  std::vector<std::filesystem::path> allowed_roots;  // where an --out symlink may point (SR-05)
};

// $HOME (when set) and the current folder.
std::vector<std::filesystem::path> default_allowed_roots();

// Default output folder: the first `<root>/run_NNN` (001, 002, ...) that does not exist (D26).
std::filesystem::path next_run_dir(const std::filesystem::path& root);

// True if path is root or inside it (both compared as given; pass canonical paths).
bool is_within(const std::filesystem::path& path, const std::filesystem::path& root);

// Checks and creates (or, with overwrite, reuses) the output folder. On failure prints one ER-01
// line to err and returns std::nullopt (the caller exits 4). Returns the absolute path.
std::optional<std::filesystem::path> prepare_out_dir(const std::filesystem::path& out,
                                                     const OutDirOptions& options,
                                                     std::ostream& err);

using WriteBody = std::function<void(std::ostream&)>;

// Creates path with O_CREAT | O_EXCL | O_NOFOLLOW, mode 0644, and writes body into it. Throws
// std::runtime_error naming the file (and the OS reason) if it cannot be created or written.
void write_result_file(const std::filesystem::path& path, const WriteBody& body);

// Writes body to an already open fd (used by write_result_file; tests pass /dev/full, D8).
void write_to_fd(int fd, const std::filesystem::path& name, const WriteBody& body);

}  // namespace frameflow
