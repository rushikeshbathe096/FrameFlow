// preview_sink.hpp
// Responsibility: the `--preview` sink (FR-CLI-07, UIUX S6, 6.2, 7.2). It stamps t_out first, so
// drawing never counts as latency, then shows the processed frame with an info panel (frame age,
// output FPS over the last second, drops, policy) and a 4 px amber border while the adaptive
// policy is SHEDDING. It shows at most 30 frames per second and skips the rest (never queues).
// Keys: q/Esc request a graceful stop, Space freezes the shown image (the pipeline keeps running
// and measuring), o toggles the panel. Drops are estimated from gaps in delivered frame ids, so
// the sink needs no shared counters (D51). The window sits behind IPreviewWindow so tests run
// without a display.
// Thread-safety: consume() runs on the worker thread only, which also owns the window (every
// HighGUI call comes from that one thread). The callbacks it calls must be thread-safe
// (Pipeline::request_stop and Pipeline::shedding are).
#pragma once

#include <functional>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <string>

#include "frameflow/clock.hpp"
#include "frameflow/sink/sink.hpp"

namespace frameflow {

class IPreviewWindow {
 public:
  virtual ~IPreviewWindow() = default;
  virtual void show(const cv::Mat& image) = 0;
  virtual int poll_key() = 0;  // key code, or -1 when no key was pressed
};

// cv::imshow / cv::waitKey(1) in a window titled "FrameFlow — <policy>" (UIUX S6).
class HighGuiWindow final : public IPreviewWindow {
 public:
  explicit HighGuiWindow(std::string title);
  ~HighGuiWindow() override;
  HighGuiWindow(const HighGuiWindow&) = delete;
  HighGuiWindow& operator=(const HighGuiWindow&) = delete;
  void show(const cv::Mat& image) override;
  int poll_key() override;

 private:
  std::string title_;
  bool created_ = false;
};

struct OverlayInfo {
  std::optional<double> age_ms;    // the shown frame's t_out - t_cap
  std::optional<double> fps;       // delivered frames in the last second
  std::optional<double> drop_pct;  // frames not delivered among ids so far
  std::string policy;
  bool shedding = false;  // amber border
  bool paused = false;    // "PAUSED" tag
  bool panel = true;      // info panel visible
};

// UIUX 6.2: black panel at 60% top-left with 12 px padding and 4 lines; 4 px amber border
// (BGR 0,159,230) while shedding; "PAUSED" centred at the top. Draws in place.
void draw_overlay(cv::Mat& canvas, const OverlayInfo& info);

class PreviewSink final : public ISink {
 public:
  PreviewSink(Clock& clock, std::unique_ptr<IPreviewWindow> window, std::string policy,
              std::function<bool()> shedding, std::function<void()> request_stop);

  void consume(Frame& frame) override;

  // The overlay of the last shown frame (for tests and inspection; worker thread only).
  const OverlayInfo& last_overlay() const { return last_info_; }

 private:
  void update_counters(const Frame& frame, Clock::time_point now);
  void handle_key(int key);
  void show_last();  // last shown image with the overlay for the current paused/panel state
  OverlayInfo overlay_for(const Frame& frame, Clock::time_point t_out) const;

  Clock& clock_;
  std::unique_ptr<IPreviewWindow> window_;
  std::string policy_;
  std::function<bool()> shedding_;
  std::function<void()> request_stop_;

  std::uint64_t delivered_ = 0;
  std::uint64_t max_id_ = 0;
  Clock::time_point fps_window_start_{};
  std::uint64_t fps_window_count_ = 0;
  std::optional<double> fps_;
  std::optional<Clock::time_point> last_shown_;
  cv::Mat last_image_;  // BGR copy of the last shown frame (shown again while paused)
  OverlayInfo last_info_;
  bool paused_ = false;
  bool panel_ = true;
  bool stop_requested_ = false;
};

}  // namespace frameflow
