// environment.hpp
// Responsibility: describes the machine a run happened on (DR-08) for `summary.json`. This is the
// only place that reads `/proc` or calls uname/gethostname (RR-12). The parsers take file
// contents as strings so tests can feed them fixed text. The hostname is stored only as an FNV-1a
// hash (SR-08, D22); no usernames or paths are recorded.
// Thread-safety: pure functions and plain data. collect_environment() reads files, so it must be
// called on the main thread before or after a run, never on the source or worker thread
// (FR-MET-07).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frameflow {

struct Environment {
  std::string cpu_model = "unknown";
  unsigned logical_cores = 0;                 // 0 when the standard library cannot tell
  std::optional<std::uint64_t> total_ram_mb;  // empty when /proc/meminfo is unreadable
  std::string kernel = "unknown";
  std::string compiler = "unknown";
  std::string opencv_version = "unknown";
  std::string hostname_hash = "unknown";
};

// The "model name" value from /proc/cpuinfo text, or "unknown" if it has none.
std::string parse_cpu_model(std::string_view cpuinfo);

// The "MemTotal" value from /proc/meminfo text (given in kB), converted to MB.
std::optional<std::uint64_t> parse_mem_total_mb(std::string_view meminfo);

// FNV-1a 64-bit hash as 16 lowercase hex digits. Privacy by default, not security (D22).
std::string hash_hostname(std::string_view hostname);

// Reads the real machine's values. Never throws; anything unreadable stays "unknown"/empty.
Environment collect_environment();

}  // namespace frameflow
