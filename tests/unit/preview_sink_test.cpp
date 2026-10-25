#include "frameflow/sink/preview_sink.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <memory>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "frameflow/platform/display.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(10);
const cv::Vec3b kAmber(0, 159, 230);
const cv::Vec3b kWhitePixel(255, 255, 255);

cv::Mat white(int width = 320, int height = 240) {
  return cv::Mat(height, width, CV_8UC3, cv::Scalar::all(255));
}

// ---------------------------------------------------------------- overlay (UIUX 6.2)

TEST(PreviewOverlayTest, PanelDarkensTopLeftAt60Percent) {
  cv::Mat canvas = white();
  OverlayInfo info;
  info.policy = "adaptive";
  draw_overlay(canvas, info);
  // Inside the panel's padding (no text there): 255 x 0.4 = 102.
  EXPECT_EQ(canvas.at<cv::Vec3b>(10, 10), cv::Vec3b(102, 102, 102));
  EXPECT_EQ(canvas.at<cv::Vec3b>(230, 310), kWhitePixel);  // far away: untouched
  EXPECT_EQ(canvas.at<cv::Vec3b>(0, 0), kWhitePixel);      // no border when not shedding
}

TEST(PreviewOverlayTest, AmberBorderOnlyWhileShedding) {
  cv::Mat canvas = white();
  OverlayInfo info;
  info.shedding = true;
  info.panel = false;
  draw_overlay(canvas, info);
  for (const cv::Point p : {cv::Point(0, 0), cv::Point(3, 120), cv::Point(319, 239)}) {
    EXPECT_EQ(canvas.at<cv::Vec3b>(p), kAmber) << p;
  }
  EXPECT_EQ(canvas.at<cv::Vec3b>(120, 4), kWhitePixel);  // exactly 4 px wide
}

TEST(PreviewOverlayTest, PanelCanBeHidden) {
  cv::Mat canvas = white();
  OverlayInfo info;
  info.panel = false;
  draw_overlay(canvas, info);
  EXPECT_EQ(cv::countNonZero(canvas.reshape(1) != 255), 0);
}

TEST(PreviewOverlayTest, PausedTagIsCentredAtTheTop) {
  cv::Mat plain = white();
  cv::Mat paused = white();
  OverlayInfo info;
  info.panel = false;
  draw_overlay(plain, info);
  info.paused = true;
  draw_overlay(paused, info);
  cv::Mat diff;
  cv::absdiff(plain, paused, diff);
  cv::cvtColor(diff, diff, cv::COLOR_BGR2GRAY);
  const cv::Rect changed = cv::boundingRect(diff > 0);
  EXPECT_GT(changed.area(), 0);
  EXPECT_LT(changed.y + changed.height, 40);              // at the top
  EXPECT_NEAR(changed.x + changed.width / 2.0, 160, 10);  // centred
}

// ---------------------------------------------------------------- sink

class FakeWindow final : public IPreviewWindow {
 public:
  FakeWindow(int& shows, std::deque<int>& keys) : shows_(shows), keys_(keys) {}
  void show(const cv::Mat& image) override {
    ++shows_;
    EXPECT_EQ(image.channels(), 3);
  }
  int poll_key() override {
    if (keys_.empty()) return -1;
    const int key = keys_.front();
    keys_.pop_front();
    return key;
  }

 private:
  int& shows_;
  std::deque<int>& keys_;
};

struct SinkFixture {
  FakeClock clock{kT0};
  int shows = 0;
  std::deque<int> keys;
  int stops = 0;
  bool shedding = false;
  PreviewSink sink{clock, std::make_unique<FakeWindow>(shows, keys), "adaptive",
                   [this] { return shedding; }, [this] { ++stops; }};

  // Delivers frame `id` captured `age` ago, as the worker would.
  Frame deliver(std::uint64_t id, milliseconds age = milliseconds(20), int channels = 3) {
    Frame frame(id, cv::Mat(48, 64, channels == 3 ? CV_8UC3 : CV_8UC1, cv::Scalar::all(0)));
    frame.meta.t_cap = clock.now() - age;
    sink.consume(frame);
    return frame;
  }
};

TEST(PreviewSinkTest, StampsOutputTimeBeforeDrawing) {
  SinkFixture f;
  const Frame frame = f.deliver(0, milliseconds(96));
  EXPECT_EQ(frame.meta.t_out, kT0);
  EXPECT_EQ(f.shows, 1);
  ASSERT_TRUE(f.sink.last_overlay().age_ms.has_value());
  EXPECT_DOUBLE_EQ(*f.sink.last_overlay().age_ms, 96.0);
  EXPECT_EQ(f.sink.last_overlay().policy, "adaptive");
}

TEST(PreviewSinkTest, ShowsAtMostAboutThirtyFramesPerSecond) {
  SinkFixture f;
  f.deliver(0);
  f.clock.advance(milliseconds(10));
  f.deliver(1);  // 10 ms later: skipped
  EXPECT_EQ(f.shows, 1);
  f.clock.advance(milliseconds(30));
  f.deliver(2);  // 40 ms after the last shown frame
  EXPECT_EQ(f.shows, 2);
}

TEST(PreviewSinkTest, DropsAreEstimatedFromIdGaps) {
  SinkFixture f;
  f.deliver(0);
  f.clock.advance(milliseconds(40));
  f.deliver(1);
  f.clock.advance(milliseconds(40));
  f.deliver(3);  // id 2 never arrived: 1 of 4 missing
  EXPECT_DOUBLE_EQ(*f.sink.last_overlay().drop_pct, 25.0);
}

TEST(PreviewSinkTest, OutputFpsOverTheLastSecond) {
  SinkFixture f;
  for (std::uint64_t id = 0; id <= 22; ++id) {  // one frame every 50 ms for 1.1 s
    f.deliver(id);
    f.clock.advance(milliseconds(50));
  }
  ASSERT_TRUE(f.sink.last_overlay().fps.has_value());
  EXPECT_NEAR(*f.sink.last_overlay().fps, 20.0, 0.5);
}

TEST(PreviewSinkTest, AmberBorderFollowsThePolicy) {
  SinkFixture f;
  f.deliver(0);
  EXPECT_FALSE(f.sink.last_overlay().shedding);
  f.shedding = true;
  f.clock.advance(milliseconds(40));
  f.deliver(1);
  EXPECT_TRUE(f.sink.last_overlay().shedding);
}

TEST(PreviewSinkTest, QuitKeysRequestOneGracefulStop) {
  for (int key : {static_cast<int>('q'), 27}) {
    SinkFixture f;
    f.keys = {key, key};
    f.deliver(0);
    f.deliver(1);
    EXPECT_EQ(f.stops, 1) << key;  // asked once, like the first Ctrl+C
  }
}

TEST(PreviewSinkTest, SpaceFreezesTheImageWhileFramesKeepFlowing) {
  SinkFixture f;
  f.keys = {' '};
  f.deliver(0);  // shown, then Space: paused (redrawn once with PAUSED)
  EXPECT_EQ(f.shows, 2);
  for (std::uint64_t id = 1; id < 5; ++id) {
    f.clock.advance(milliseconds(40));
    const Frame frame = f.deliver(id);
    EXPECT_TRUE(frame.meta.t_out.has_value());  // still delivered and measured
  }
  EXPECT_EQ(f.shows, 2);  // but not shown
  f.keys = {' '};
  f.clock.advance(milliseconds(40));
  f.deliver(5);  // Space again: unfreeze (redraw) ...
  f.clock.advance(milliseconds(40));
  f.deliver(6);  // ... and the next frame is shown
  EXPECT_EQ(f.shows, 4);
}

TEST(PreviewSinkTest, OKeyTogglesThePanel) {
  SinkFixture f;
  f.keys = {'o'};
  f.deliver(0);
  f.clock.advance(milliseconds(40));
  f.deliver(1);
  f.keys = {'o'};
  f.clock.advance(milliseconds(40));
  f.deliver(2);
  EXPECT_EQ(f.shows, 3);  // toggling never stops frames from being shown
}

TEST(PreviewSinkTest, GreyFramesAreShownInColour) {
  SinkFixture f;
  f.deliver(0, milliseconds(5), 1);  // FakeWindow checks 3 channels
  EXPECT_EQ(f.shows, 1);
}

// ---------------------------------------------------------------- display check (ER-06)

TEST(DisplayTest, AvailableWhenEitherVariableIsSet) {
  EXPECT_TRUE(display_available(":0", nullptr));
  EXPECT_TRUE(display_available(nullptr, "wayland-0"));
  EXPECT_FALSE(display_available(nullptr, nullptr));
  EXPECT_FALSE(display_available("", ""));
}

}  // namespace
}  // namespace frameflow
