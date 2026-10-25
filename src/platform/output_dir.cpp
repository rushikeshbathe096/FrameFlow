#include "frameflow/platform/output_dir.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>
#include <utility>

namespace frameflow {

namespace fs = std::filesystem;

namespace {

// A std::streambuf that writes straight to a file descriptor, so a result file is written
// through the same fd that O_EXCL created (no second open by name, SR-06). Remembers errno.
class FdStreamBuf final : public std::streambuf {
 public:
  explicit FdStreamBuf(int fd) : fd_(fd) { setp(buffer_.data(), buffer_.data() + buffer_.size()); }
  int error() const { return error_; }

 protected:
  int_type overflow(int_type ch) override {
    if (!flush_buffer()) return traits_type::eof();
    if (!traits_type::eq_int_type(ch, traits_type::eof())) {
      *pptr() = traits_type::to_char_type(ch);
      pbump(1);
    }
    return traits_type::not_eof(ch);
  }
  int sync() override { return flush_buffer() ? 0 : -1; }

 private:
  bool flush_buffer() {
    const char* data = pbase();
    std::size_t left = static_cast<std::size_t>(pptr() - pbase());
    while (left > 0) {
      const ssize_t written = ::write(fd_, data, left);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) {
        error_ = written < 0 ? errno : EIO;
        return false;
      }
      data += written;
      left -= static_cast<std::size_t>(written);
    }
    setp(buffer_.data(), buffer_.data() + buffer_.size());
    return true;
  }

  int fd_;
  int error_ = 0;
  std::array<char, std::size_t{64} * 1024> buffer_{};
};

// Closes the fd on every path, including exceptions from the write body.
class FdGuard {
 public:
  explicit FdGuard(int fd) : fd_(fd) {}
  ~FdGuard() {
    if (fd_ >= 0) ::close(fd_);
  }
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  int release() { return std::exchange(fd_, -1); }

 private:
  int fd_;
};

// Thread-safe replacement for strerror (concurrency-mt-unsafe).
std::string os_message(int error) { return std::generic_category().message(error); }

void print_error(std::ostream& err, const std::string& what, const std::string& hint) {
  err << "frameflow: error: " << what << " (hint: " << hint << ")\n";
}

fs::path absolute_dir(const fs::path& out) {
  fs::path path = fs::absolute(out).lexically_normal();
  if (path.filename().empty()) path = path.parent_path();  // "a/b/" -> "a/b"
  return path;
}

// SR-05: if a symlink anywhere in --out changes where it resolves to, the resolved folder must
// be inside one of the allowed roots (D31).
bool symlink_allowed(const fs::path& dir, const OutDirOptions& options, std::ostream& err) {
  std::error_code ec;
  const fs::path target = fs::weakly_canonical(dir, ec);
  if (options.allow_any_out || (!ec && target == dir)) return true;
  for (const fs::path& root : options.allowed_roots) {
    std::error_code root_ec;
    const fs::path canonical_root = fs::weakly_canonical(root, root_ec);
    if (!ec && !root_ec && is_within(target, canonical_root)) return true;
  }
  print_error(err,
              "output folder resolves through a symlink to outside your home and the current "
              "folder: " +
                  dir.string() + " -> " + target.string(),
              "use a real folder, or add --allow-any-out");
  return false;
}

// --overwrite: remove only FrameFlow's own result files (unlinks a symlink, never its target).
bool remove_old_results(const fs::path& dir, std::ostream& err) {
  for (const char* name : kResultFiles) {
    std::error_code ec;
    fs::remove(dir / name, ec);
    if (ec) {
      print_error(err, "cannot replace " + (dir / name).string() + ": " + ec.message(),
                  "check the file's permissions");
      return false;
    }
  }
  return true;
}

bool reuse_existing(const fs::path& dir, const OutDirOptions& options, std::ostream& err) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    print_error(err, "output path exists and is not a folder: " + dir.string(),
                "choose another --out");
    return false;
  }
  if (!options.overwrite) {
    print_error(err, "output folder exists: " + dir.string(), "use --overwrite or a new --out");
    return false;
  }
  return remove_old_results(dir, err);
}

bool create_dir(const fs::path& dir, std::ostream& err) {
  std::error_code ec;
  if (!fs::is_directory(dir.parent_path(), ec)) {
    print_error(err, "output folder's parent does not exist: " + dir.parent_path().string(),
                "create it first or choose another --out");
    return false;
  }
  if (::mkdir(dir.c_str(), 0755) != 0) {  // umask applies (SR-AUTH-04)
    const int error = errno;
    print_error(err, "cannot create output folder: " + dir.string() + ": " + os_message(error),
                error == EACCES ? "check that you can write to " + dir.parent_path().string()
                                : "choose another --out");
    return false;
  }
  return true;
}

}  // namespace

std::vector<fs::path> default_allowed_roots() {
  std::vector<fs::path> roots;
  // NOLINTNEXTLINE(concurrency-mt-unsafe): main thread, before any pipeline thread; no setenv
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    roots.emplace_back(home);
  }
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) roots.push_back(cwd);
  return roots;
}

fs::path next_run_dir(const fs::path& root) {
  std::error_code ec;
  for (int n = 1;; ++n) {
    char name[16];
    (void)std::snprintf(name, sizeof(name), "run_%03d", n);
    const fs::path candidate = root / name;
    if (!fs::exists(fs::symlink_status(candidate, ec))) return candidate;
  }
}

bool is_within(const fs::path& path, const fs::path& root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p) {
    if (r->empty()) continue;  // trailing separator in root
    if (p == path.end() || *p != *r) return false;
  }
  return true;
}

std::optional<fs::path> prepare_out_dir(const fs::path& out, const OutDirOptions& options,
                                        std::ostream& err) {
  const fs::path dir = absolute_dir(out);
  if (!symlink_allowed(dir, options, err)) return std::nullopt;
  std::error_code ec;
  const bool exists = fs::exists(fs::symlink_status(dir, ec));
  const bool ok = exists ? reuse_existing(dir, options, err) : create_dir(dir, err);
  if (!ok) return std::nullopt;
  return dir;
}

void write_to_fd(int fd, const fs::path& name, const WriteBody& body) {
  FdStreamBuf buffer(fd);
  std::ostream out(&buffer);
  body(out);
  out.flush();
  if (!out) {
    throw std::runtime_error("cannot write " + name.string() + ": " +
                             os_message(buffer.error() != 0 ? buffer.error() : EIO));
  }
}

void write_result_file(const fs::path& path, const WriteBody& body) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) {
    throw std::runtime_error("cannot create " + path.string() + ": " + os_message(errno));
  }
  FdGuard guard(fd);
  write_to_fd(fd, path, body);
  if (::close(guard.release()) != 0) {
    throw std::runtime_error("cannot write " + path.string() + ": " + os_message(errno));
  }
}

}  // namespace frameflow
