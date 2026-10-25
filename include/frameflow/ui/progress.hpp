// progress.hpp
// Responsibility: the `--progress` status line (UIUX S3, D6, D52), e.g.
//   [####------]  24/60 s  ·  out 19.8 fps  ·  queue 8/8  ·  drops 33%  ·  age 96 ms  ·  SHEDDING
// During warm-up the metrics are replaced by a `warm-up` badge (UIUX 9.1). Output FPS is the
// delivered count between two ticks; drops are dropped / (delivered + dropped) so far. In a
// terminal the line is rewritten in place (`\r` + clear), otherwise every tick is a new line.
// Thread-safety: ProgressLine is used by one thread, the one running Pipeline::run() (main);
// it formats a snapshot the pipeline built from atomic counters (no I/O on pipeline threads).
#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "frameflow/pipeline.hpp"

namespace frameflow {

struct ProgressFrame {
  Clock::duration total{0};  // run length (duration, or frames / fps)
  Clock::duration warmup{0};
  std::size_t capacity = 0;  // queue capacity
};

// One line, without `\r` or `\n`.
std::string format_progress(const Pipeline::LiveStatus& status, const ProgressFrame& frame,
                            std::optional<double> out_fps);

class ProgressLine {
 public:
  ProgressLine(ProgressFrame frame, bool terminal) : frame_(frame), terminal_(terminal) {}

  // The text to write for this tick (with `\r\033[K` in a terminal, `\n` otherwise).
  std::string update(const Pipeline::LiveStatus& status);
  // What to write after the run so the next output starts on a fresh line.
  std::string finish() const { return terminal_ && printed_ ? "\n" : ""; }

 private:
  ProgressFrame frame_;
  bool terminal_;
  bool printed_ = false;
  std::optional<Pipeline::LiveStatus> previous_;
};

}  // namespace frameflow
