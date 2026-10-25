#include "frameflow/ui/progress.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>

namespace frameflow {

namespace {

constexpr int kBarWidth = 10;

std::string fixed(double value, int decimals) {
  char buffer[48];
  const std::to_chars_result r =
      std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, decimals);
  return std::string(buffer, r.ptr);
}

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

std::string bar(Clock::duration elapsed, Clock::duration total) {
  const double fraction =
      total > Clock::duration::zero() ? std::clamp(seconds(elapsed) / seconds(total), 0.0, 1.0) : 0;
  const int filled = static_cast<int>(std::floor(fraction * kBarWidth));
  return "[" + std::string(static_cast<std::size_t>(filled), '#') +
         std::string(static_cast<std::size_t>(kBarWidth - filled), '-') + "]";
}

}  // namespace

std::string format_progress(const Pipeline::LiveStatus& status, const ProgressFrame& frame,
                            std::optional<double> out_fps) {
  const long elapsed_s = static_cast<long>(std::floor(seconds(status.elapsed)));
  const long total_s = static_cast<long>(std::ceil(seconds(frame.total)));
  std::string line = bar(status.elapsed, frame.total) + "  " + std::to_string(elapsed_s) + "/" +
                     std::to_string(total_s) + " s";
  if (status.elapsed < frame.warmup) return line + "  ·  warm-up";
  const LiveCounts& c = status.counts;
  const std::uint64_t finished = c.delivered + c.dropped;
  line += "  ·  out " + (out_fps ? fixed(*out_fps, 1) : std::string("-")) + " fps";
  line += "  ·  queue " + std::to_string(c.depth) + "/" + std::to_string(frame.capacity);
  line +=
      "  ·  drops " +
      (finished ? fixed(100.0 * static_cast<double>(c.dropped) / static_cast<double>(finished), 0)
                : "0") +
      "%";
  line +=
      "  ·  age " +
      (c.last_latency ? fixed(std::chrono::duration<double, std::milli>(*c.last_latency).count(), 0)
                      : std::string("-")) +
      " ms";
  if (status.shedding) line += "  ·  SHEDDING";
  return line;
}

std::string ProgressLine::update(const Pipeline::LiveStatus& status) {
  std::optional<double> fps;
  if (previous_ && status.elapsed > previous_->elapsed) {
    fps = static_cast<double>(status.counts.delivered - previous_->counts.delivered) /
          seconds(status.elapsed - previous_->elapsed);
  }
  previous_ = status;
  printed_ = true;
  const std::string line = format_progress(status, frame_, fps);
  return terminal_ ? "\r\033[K" + line : line + "\n";
}

}  // namespace frameflow
