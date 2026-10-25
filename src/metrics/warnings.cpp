#include "frameflow/metrics/warnings.hpp"

#include <charconv>
#include <chrono>
#include <optional>

namespace frameflow {

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr milliseconds kLateThreshold{5};
constexpr double kLateFractionLimit = 0.01;
constexpr seconds kShortRun{10};

std::string fixed1(double value) {
  char buffer[48];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 1);
  return std::string(buffer, result.ptr);
}

std::string number_text(double value) {
  char buffer[32];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

// Over every produced frame (lateness is a property of the source, not of the window). Only the
// source's own lateness counts: waiting on a full queue under blocking is backpressure, the
// policy working as designed, not an overloaded system (D38).
std::optional<std::string> late_source(const RunReport& report) {
  if (report.frames.empty()) return std::nullopt;
  std::size_t late = 0;
  for (const FrameMeta& meta : report.frames) {
    if (meta.own_lateness() > kLateThreshold) ++late;
  }
  const double fraction = static_cast<double>(late) / static_cast<double>(report.frames.size());
  if (fraction <= kLateFractionLimit) return std::nullopt;
  return "source was late by > 5 ms on " + fixed1(fraction * 100.0) +
         "% of frames; system may be overloaded";
}

}  // namespace

std::vector<std::string> collect_warnings(const RunConfig& config, const RunReport& report) {
  std::vector<std::string> warnings;
  if (auto late = late_source(report)) warnings.push_back(*late);
  if (!report.valid && report.ended_by != EndedBy::Error) {
    warnings.emplace_back("stopped during warm-up; results marked invalid");
  }
  if (config.warmup_s > config.run_seconds() / 2.0) {
    warnings.push_back("warm-up (" + number_text(config.warmup_s) +
                       " s) is longer than half the run (" + number_text(config.run_seconds()) +
                       " s); few frames are measured");
  }
  if (report.t_end - report.t0 < kShortRun) {
    warnings.emplace_back("run shorter than 10 s; percentiles may be unstable");
  }
  return warnings;
}

}  // namespace frameflow
