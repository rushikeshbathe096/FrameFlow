#include "frameflow/platform/environment.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <charconv>
#include <climits>
#include <cstdio>
#include <fstream>
#include <opencv2/core/version.hpp>
#include <sstream>
#include <thread>

namespace frameflow {

namespace {

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) return {};
  const auto last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

// The value after "key:" on the first line whose key matches, if any.
std::optional<std::string_view> find_value(std::string_view text, std::string_view key) {
  while (!text.empty()) {
    const auto end = text.find('\n');
    const std::string_view line = text.substr(0, end);
    const auto colon = line.find(':');
    if (colon != std::string_view::npos && trim(line.substr(0, colon)) == key) {
      return trim(line.substr(colon + 1));
    }
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
  return std::nullopt;
}

std::string read_file(const char* path) {
  std::ifstream in(path);
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

std::string kernel_release() {
  utsname info{};
  if (uname(&info) != 0) return "unknown";
  return info.release;
}

std::string hostname() {
  char name[HOST_NAME_MAX + 1] = {};
  if (gethostname(name, sizeof(name) - 1) != 0) return {};
  return name;
}

std::string compiler_version() {
#if defined(__clang__)
  return "clang " __clang_version__;
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#else
  return "unknown";
#endif
}

}  // namespace

std::string parse_cpu_model(std::string_view cpuinfo) {
  const auto value = find_value(cpuinfo, "model name");
  if (!value || value->empty()) return "unknown";
  return std::string(*value);
}

std::optional<std::uint64_t> parse_kb_field(std::string_view text, std::string_view key) {
  const auto value = find_value(text, key);
  if (!value) return std::nullopt;
  std::uint64_t kb = 0;
  const auto [ptr, ec] = std::from_chars(value->data(), value->data() + value->size(), kb);
  if (ec != std::errc() ||
      trim(std::string_view(ptr, value->data() + value->size() - ptr)) != "kB") {
    return std::nullopt;
  }
  return kb;
}

std::optional<std::uint64_t> parse_mem_total_mb(std::string_view meminfo) {
  const std::optional<std::uint64_t> kb = parse_kb_field(meminfo, "MemTotal");
  if (!kb) return std::nullopt;
  return *kb / 1024;
}

MemoryStatus read_memory_status() {
  const std::string status = read_file("/proc/self/status");
  return MemoryStatus{parse_kb_field(status, "VmRSS"), parse_kb_field(status, "VmHWM")};
}

std::string hash_hostname(std::string_view hostname) {
  std::uint64_t hash = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
  for (const char c : hostname) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ULL;  // FNV-1a 64-bit prime
  }
  char hex[17];
  (void)std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
  return hex;
}

Environment collect_environment() {
  Environment env;
  env.cpu_model = parse_cpu_model(read_file("/proc/cpuinfo"));
  env.logical_cores = std::thread::hardware_concurrency();
  env.total_ram_mb = parse_mem_total_mb(read_file("/proc/meminfo"));
  env.kernel = kernel_release();
  env.compiler = compiler_version();
  env.opencv_version = CV_VERSION;
  const std::string host = hostname();
  if (!host.empty()) env.hostname_hash = hash_hostname(host);
  return env;
}

}  // namespace frameflow
