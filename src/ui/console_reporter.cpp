#include "frameflow/ui/console_reporter.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>

namespace frameflow {

namespace {

const char* const kNone = "—";  // UIUX 9.4: a value that does not exist (not 0)

std::string fixed(double value, int decimals) {
  char buffer[48];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, decimals);
  return std::string(buffer, result.ptr);
}

std::string fixed1(const std::optional<double>& value) { return value ? fixed(*value, 1) : kNone; }

std::string number_text(double value) {
  char buffer[32];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

struct ReasonCounts {
  std::uint64_t oldest = 0, newest = 0, stale = 0;
};

ReasonCounts count_reasons(const std::vector<FrameMeta>& frames) {
  ReasonCounts counts;
  for (const FrameMeta& meta : frames) {
    if (meta.drop_reason == DropReason::Oldest) ++counts.oldest;
    if (meta.drop_reason == DropReason::Newest) ++counts.newest;
    if (meta.drop_reason == DropReason::Stale) ++counts.stale;
  }
  return counts;
}

void put_latency(std::ostream& out, const RunStats& s) {
  out << "  Latency (ms)     ";
  if (!s.p50_ms) {
    out << kNone << "  no frames delivered\n";
    return;
  }
  out << "p50 " << fixed1(s.p50_ms) << "   p95 " << fixed1(s.p95_ms) << "   p99 "
      << fixed1(s.p99_ms) << "   max " << fixed1(s.max_ms) << '\n';
}

void put_frames(std::ostream& out, const RunReport& report) {
  const RunStats& s = report.stats;
  out << "  Frames           produced " << s.produced << "   delivered " << s.delivered
      << "   dropped " << s.dropped;
  if (s.drop_rate_pct) out << " (" << fixed(*s.drop_rate_pct, 1) << "%)";
  out << "   in-flight " << s.in_flight << '\n';
  if (s.dropped == 0) return;  // UIUX 9.4: reason breakdown omitted without drops
  const ReasonCounts reasons = count_reasons(report.frames);
  out << "                   oldest " << reasons.oldest << "   newest " << reasons.newest
      << "   stale " << reasons.stale << '\n';
}

void put_depth(std::ostream& out, const RunConfig& config, const RunStats& s) {
  out << "  Queue depth      mean " << fixed1(s.depth_mean) << "   peak ";
  if (s.depth_max) {
    out << *s.depth_max;
  } else {
    out << kNone;
  }
  out << " / " << config.queue << '\n';
}

}  // namespace

std::string format_run_summary(const RunConfig& config, const RunReport& report) {
  const RunStats& s = report.stats;
  std::ostringstream out;
  out << "Result  ·  policy " << config.policy << "  ·  " << overload_text(config) << "  ·  "
      << (report.valid ? "valid" : "invalid") << "\n\n";
  put_latency(out, s);
  out << "  Throughput       in " << fixed1(s.input_fps) << " fps   out " << fixed1(s.output_fps)
      << " fps\n";
  put_frames(out, report);
  put_depth(out, config, s);
  if (s.state_changes) {  // UIUX S4 / 9.4: the Adaptive row only for the adaptive policy
    out << "  Adaptive         " << *s.state_changes
        << (*s.state_changes == 1 ? " state change" : " state changes") << "   time shedding "
        << fixed1(s.shedding_pct) << "%\n";
  }
  if (config.warmup_s > 0) {
    out << "  (frame counts: whole run; latency, fps, drop % and depth: after the "
        << number_text(config.warmup_s) << " s warm-up)\n";
  }
  return out.str();
}

std::string overload_text(const RunConfig& config) {
  const std::string text = "overload " + fixed(config.cost_ms * config.fps / 1000.0, 2) + "x";
  return config.processor == "none" ? text : text + " (cost only)";
}

std::string format_warnings(const std::vector<std::string>& warnings) {
  std::string text;
  for (const std::string& warning : warnings) text += "frameflow: warning: " + warning + '\n';
  return text;
}

}  // namespace frameflow
