#include "frameflow/sink/preview_sink.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <utility>
#include <vector>

namespace frameflow {

namespace {

using std::chrono::milliseconds;

constexpr milliseconds kMinShowInterval{33};  // at most ~30 shown frames per second
cv::Scalar amber() { return {0, 159, 230}; }  // BGR of #E69F00 (UIUX 12.2)
cv::Scalar white() { return {255, 255, 255}; }
constexpr int kPadding = 12;
constexpr int kBorder = 4;
constexpr int kLineHeight = 20;
constexpr int kKeyEscape = 27;

std::string fixed(const std::optional<double>& value, int decimals, const char* unit) {
  if (!value) return "-";
  char buffer[48];
  const std::to_chars_result r =
      std::to_chars(buffer, buffer + sizeof(buffer), *value, std::chars_format::fixed, decimals);
  return std::string(buffer, r.ptr) + unit;
}

// Grey processor outputs (grayscale, canny) become BGR so the amber border can be drawn.
cv::Mat to_bgr(const cv::Mat& image) {
  cv::Mat bgr;
  if (image.channels() == 1) {
    cv::cvtColor(image, bgr, cv::COLOR_GRAY2BGR);
  } else {
    bgr = image.clone();
  }
  return bgr;
}

// Exactly kBorder pixels on every side (cv::rectangle centres thick lines on the edge).
void draw_border(cv::Mat& canvas) {
  const int width = std::min({kBorder, canvas.cols, canvas.rows});
  canvas.rowRange(0, width).setTo(amber());
  canvas.rowRange(canvas.rows - width, canvas.rows).setTo(amber());
  canvas.colRange(0, width).setTo(amber());
  canvas.colRange(canvas.cols - width, canvas.cols).setTo(amber());
}

void draw_panel(cv::Mat& canvas, const OverlayInfo& info) {
  const std::vector<std::string> lines = {
      "Age     " + fixed(info.age_ms, 0, " ms"), "FPS     " + fixed(info.fps, 1, ""),
      "Drops   " + fixed(info.drop_pct, 0, "%"), "Policy  " + info.policy};
  const cv::Rect box(kBorder + 4, kBorder + 4, 2 * kPadding + 170,
                     2 * kPadding + kLineHeight * static_cast<int>(lines.size()));
  const cv::Rect clipped = box & cv::Rect(0, 0, canvas.cols, canvas.rows);
  if (clipped.empty()) return;
  cv::Mat region = canvas(clipped);
  region.convertTo(region, -1, 0.4, 0);  // black at 60% opacity over the frame
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const cv::Point origin(box.x + kPadding,
                           box.y + kPadding + kLineHeight * static_cast<int>(i + 1) - 5);
    cv::putText(canvas, lines[i], origin, cv::FONT_HERSHEY_SIMPLEX, 0.5, white(), 1, cv::LINE_AA);
  }
}

}  // namespace

HighGuiWindow::HighGuiWindow(std::string title) : title_(std::move(title)) {}

HighGuiWindow::~HighGuiWindow() {
  if (created_) cv::destroyWindow(title_);
}

void HighGuiWindow::show(const cv::Mat& image) {
  if (!created_) {
    cv::namedWindow(title_, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);  // aspect ratio kept
    created_ = true;
  }
  cv::imshow(title_, image);
}

int HighGuiWindow::poll_key() { return cv::waitKey(1); }  // UIUX 7.2: 1 ms on the sink side

void draw_overlay(cv::Mat& canvas, const OverlayInfo& info) {
  if (info.panel) draw_panel(canvas, info);
  if (info.shedding) draw_border(canvas);
  if (info.paused) {
    int baseline = 0;
    const cv::Size size = cv::getTextSize("PAUSED", cv::FONT_HERSHEY_SIMPLEX, 0.7, 2, &baseline);
    const cv::Point origin((canvas.cols - size.width) / 2, kBorder + 24);
    // Black outline under white text: readable on any frame.
    cv::putText(canvas, "PAUSED", origin, cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar::all(0), 4,
                cv::LINE_AA);
    cv::putText(canvas, "PAUSED", origin, cv::FONT_HERSHEY_SIMPLEX, 0.7, white(), 2, cv::LINE_AA);
  }
}

PreviewSink::PreviewSink(Clock& clock, std::unique_ptr<IPreviewWindow> window, std::string policy,
                         std::function<bool()> shedding, std::function<void()> request_stop)
    : clock_(clock),
      window_(std::move(window)),
      policy_(std::move(policy)),
      shedding_(std::move(shedding)),
      request_stop_(std::move(request_stop)) {}

void PreviewSink::consume(Frame& frame) {
  const Clock::time_point now = clock_.now();
  frame.meta.t_out = now;  // first: drawing is not latency
  update_counters(frame, now);
  if (!paused_ && (!last_shown_ || now - *last_shown_ >= kMinShowInterval)) {
    last_image_ = to_bgr(frame.image);  // a copy: the frame is freed after the sink
    last_info_ = overlay_for(frame, now);
    show_last();
    last_shown_ = now;
  }
  handle_key(window_->poll_key());
}

void PreviewSink::show_last() {
  if (last_image_.empty()) return;
  cv::Mat canvas = last_image_.clone();
  OverlayInfo info = last_info_;
  info.paused = paused_;
  info.panel = panel_;
  draw_overlay(canvas, info);
  window_->show(canvas);
}

void PreviewSink::update_counters(const Frame& frame, Clock::time_point now) {
  ++delivered_;
  max_id_ = std::max(max_id_, frame.meta.id);
  if (fps_window_count_ == 0) fps_window_start_ = now;
  ++fps_window_count_;
  const Clock::duration elapsed = now - fps_window_start_;
  if (elapsed >= std::chrono::seconds(1)) {
    fps_ =
        static_cast<double>(fps_window_count_ - 1) / std::chrono::duration<double>(elapsed).count();
    fps_window_start_ = now;
    fps_window_count_ = 1;
  }
}

OverlayInfo PreviewSink::overlay_for(const Frame& frame, Clock::time_point t_out) const {
  OverlayInfo info;
  if (frame.meta.t_cap) {
    info.age_ms = std::chrono::duration<double, std::milli>(t_out - *frame.meta.t_cap).count();
  }
  info.fps = fps_;
  const double seen = static_cast<double>(max_id_ + 1);  // ids 0..max_id were produced
  info.drop_pct = 100.0 * (seen - static_cast<double>(delivered_)) / seen;
  info.policy = policy_;
  info.shedding = shedding_ && shedding_();
  return info;
}

void PreviewSink::handle_key(int key) {
  if (key < 0) return;
  const int code = key & 0xFF;
  if ((code == 'q' || code == kKeyEscape) && !stop_requested_) {
    stop_requested_ = true;  // same as Ctrl+C (UIUX 7.2)
    if (request_stop_) request_stop_();
  } else if (code == ' ') {
    paused_ = !paused_;
    show_last();  // the frozen image with (or without) the PAUSED tag
  } else if (code == 'o') {
    panel_ = !panel_;
    if (paused_) show_last();  // otherwise the next shown frame picks it up
  }
}

}  // namespace frameflow
