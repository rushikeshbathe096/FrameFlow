// preview_sink.hpp
// Responsibility: `--preview` (FR-CLI-07, UIUX S6, 6.2, 7.2), split across two threads (D57).
// PreviewSink runs on the worker: it stamps t_out, updates its counters (output FPS over the last
// second, drops estimated from gaps in delivered ids, D51) and posts the frame's image plus the
// overlay data into a PreviewMailbox. It never calls HighGUI and never waits. PreviewDisplay runs
// on the main thread, which owns the window: about every 15 ms it takes the newest item, draws
// the info panel (age, FPS, drops, policy), the 4 px amber border while the adaptive policy is
// SHEDDING and the PAUSED tag, shows it, and handles keys: q/Esc or closing the window request a
// graceful stop, Space freezes the shown image (the pipeline keeps running and measuring), o
// toggles the panel. The window sits behind IPreviewWindow so tests run without a display.
// Thread-safety: PreviewSink is used by the worker thread only; PreviewDisplay and the window by
// the main thread only; PreviewMailbox is shared and guarded by its mutex (held only to move one
// item in or out). Images are shared, not copied: nothing writes a frame's pixels after the sink,
// and cv::Mat reference counts are atomic.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
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
  virtual int poll_key() = 0;       // key code, or -1 when no key was pressed
  virtual bool closed() const = 0;  // true once the user has closed a shown window
};

// cv::imshow / cv::waitKey(1) in a window titled "FrameFlow — <policy>" (UIUX S6). Created on
// the first show() and destroyed by the destructor: both on the main thread (D57).
class HighGuiWindow final : public IPreviewWindow {
 public:
  explicit HighGuiWindow(std::string title);
  ~HighGuiWindow() override;
  HighGuiWindow(const HighGuiWindow&) = delete;
  HighGuiWindow& operator=(const HighGuiWindow&) = delete;
  void show(const cv::Mat& image) override;
  int poll_key() override;
  bool closed() const override;

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

struct PreviewItem {
  cv::Mat image;  // shares the frame's pixels (no copy)
  OverlayInfo info;
};

// One slot: post() replaces whatever is there (older frames are dropped, never queued), take()
// empties it. post() only waits for the mutex, which take() holds just to move one item out.
class PreviewMailbox {
 public:
  void post(PreviewItem item);
  std::optional<PreviewItem> take();

 private:
  std::mutex mutex_;
  std::optional<PreviewItem> slot_;
};

class PreviewSink final : public ISink {
 public:
  PreviewSink(Clock& clock, PreviewMailbox& mailbox, std::string policy,
              std::function<bool()> shedding);

  void consume(Frame& frame) override;

  // The overlay of the last posted frame (for tests and inspection; worker thread only).
  const OverlayInfo& last_overlay() const { return last_info_; }

 private:
  void update_counters(const Frame& frame, Clock::time_point now);
  OverlayInfo overlay_for(const Frame& frame, Clock::time_point t_out) const;

  Clock& clock_;
  PreviewMailbox& mailbox_;
  std::string policy_;
  std::function<bool()> shedding_;

  std::uint64_t delivered_ = 0;
  std::uint64_t max_id_ = 0;
  Clock::time_point fps_window_start_{};
  std::uint64_t fps_window_count_ = 0;
  std::optional<double> fps_;
  OverlayInfo last_info_;
};

// Main thread: called about every 15 ms while the run is active (D57).
class PreviewDisplay {
 public:
  static constexpr std::chrono::milliseconds kInterval{15};

  PreviewDisplay(IPreviewWindow& window, PreviewMailbox& mailbox,
                 std::function<void()> request_stop);

  // Shows the newest posted frame (unless paused) and handles keys and the close button.
  void pump();

  bool paused() const { return paused_; }
  bool panel() const { return panel_; }

 private:
  void handle_key(int key);
  void show_last();  // last taken image with the overlay for the current paused/panel state

  IPreviewWindow& window_;
  PreviewMailbox& mailbox_;
  std::function<void()> request_stop_;
  cv::Mat last_image_;  // BGR copy of the last shown frame (shown again while paused)
  OverlayInfo last_info_;
  bool paused_ = false;
  bool panel_ = true;
  bool stop_requested_ = false;
};

}  // namespace frameflow
