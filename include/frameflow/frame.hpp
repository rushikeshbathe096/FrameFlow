// frame.hpp
// Responsibility: one video frame (pixels + per-frame metadata) and its owning pointer type.
// A Frame is move-only: copying a cv::Mat shares its pixel buffer, so a copied Frame would let
// two threads touch the same pixels. Frames travel source -> queue -> worker -> sink as FramePtr
// (std::unique_ptr), so exactly one owner exists at any time.
// Thread-safety: none internally. A Frame is used by one thread at a time; ownership is handed
// over through the queue, whose mutex orders the previous owner's writes before the next
// owner's reads.
#pragma once

#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <string_view>
#include <utility>

#include "frameflow/clock.hpp"

namespace frameflow {

enum class DropReason { None, Oldest, Newest, Stale };

// Name used in result files (`reason` column, events): "", "oldest", "newest", "stale".
constexpr std::string_view drop_reason_name(DropReason reason) {
  switch (reason) {
    case DropReason::Oldest:
      return "oldest";
    case DropReason::Newest:
      return "newest";
    case DropReason::Stale:
      return "stale";
    case DropReason::None:
      break;
  }
  return "";
}

// Per-frame measurements (SRS 6.1). An empty timestamp means the frame never reached that stage.
struct FrameMeta {
  std::uint64_t id = 0;
  std::optional<Clock::time_point> t_cap;
  std::optional<Clock::time_point> t_push;
  std::optional<Clock::time_point> t_pop;
  std::optional<Clock::time_point> t_proc_start;
  std::optional<Clock::time_point> t_proc_end;
  std::optional<Clock::time_point> t_out;
  std::optional<Clock::time_point> t_drop;  // when a policy dropped it (FR-POL-10)
  Clock::duration source_lateness{0};       // actual release - scheduled release (FR-SRC-07)
  // The part of source_lateness caused by the source waiting inside earlier push() calls
  // (blocking backpressure), not by the source itself being slow (D38). Not in frames.csv.
  Clock::duration backpressure{0};
  DropReason drop_reason = DropReason::None;  // single source of truth for "dropped"
  bool warmup = false;

  bool is_dropped() const { return drop_reason != DropReason::None; }
  // Lateness the source caused itself (CPU, slow decode, oversleeping), used by ER-07.
  Clock::duration own_lateness() const { return source_lateness - backpressure; }
};

class Frame {
 public:
  Frame(std::uint64_t id, cv::Mat image) : image(std::move(image)) { meta.id = id; }

  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  Frame(Frame&&) noexcept = default;
  Frame& operator=(Frame&&) noexcept = default;
  ~Frame() = default;

  cv::Mat image;
  FrameMeta meta;
};

using FramePtr = std::unique_ptr<Frame>;

}  // namespace frameflow
