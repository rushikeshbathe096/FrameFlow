#include "frameflow/sink/preview_sink.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <memory>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <thread>
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

// ---------------------------------------------------------------- mailbox (D57)

PreviewItem item(std::uint64_t id) {
  PreviewItem out;
  out.image = cv::Mat(4, 4, CV_8UC3, cv::Scalar::all(static_cast<double>(id)));
  out.info.age_ms = static_cast<double>(id);
  return out;
}

TEST(PreviewMailboxTest, KeepsOnlyTheNewestItem) {
  PreviewMailbox mailbox;
  EXPECT_FALSE(mailbox.take().has_value());
  for (std::uint64_t id = 0; id < 5; ++id) mailbox.post(item(id));
  const std::optional<PreviewItem> newest = mailbox.take();
  ASSERT_TRUE(newest.has_value());
  EXPECT_EQ(newest->info.age_ms, 4.0);
  EXPECT_FALSE(mailbox.take().has_value());  // taking empties the slot: nothing is queued
}

// The worker posts without ever waiting for the display: with nobody taking, 100000 posts finish
// at once and hold one item.
TEST(PreviewMailboxTest, PostNeverWaitsForTheReader) {
  PreviewMailbox mailbox;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t id = 0; id < 100'000; ++id) mailbox.post(item(id % 256));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  EXPECT_TRUE(mailbox.take().has_value());
  EXPECT_FALSE(mailbox.take().has_value());
}

// Worker and main thread at the same time (run under TSan): the reader only ever sees newer
// items and finally the last one.
TEST(PreviewMailboxTest, ConcurrentPostAndTake) {
  PreviewMailbox mailbox;
  constexpr int kItems = 20'000;
  std::thread worker([&] {
    for (int id = 1; id <= kItems; ++id) mailbox.post(item(static_cast<std::uint64_t>(id)));
  });
  double last_seen = 0;
  while (last_seen < kItems) {
    if (const std::optional<PreviewItem> taken = mailbox.take()) {
      ASSERT_GT(*taken->info.age_ms, last_seen);
      last_seen = *taken->info.age_ms;
    }
  }
  worker.join();
  EXPECT_EQ(last_seen, kItems);
}

// ---------------------------------------------------------------- sink (worker side)

struct SinkFixture {
  FakeClock clock{kT0};
  PreviewMailbox mailbox;
  bool shedding = false;
  PreviewSink sink{clock, mailbox, "adaptive", [this] { return shedding; }};

  // Delivers frame `id` captured `age` ago, as the worker would.
  Frame deliver(std::uint64_t id, milliseconds age = milliseconds(20), int channels = 3) {
    Frame frame(id, cv::Mat(48, 64, channels == 3 ? CV_8UC3 : CV_8UC1, cv::Scalar::all(0)));
    frame.meta.t_cap = clock.now() - age;
    sink.consume(frame);
    return frame;
  }
};

TEST(PreviewSinkTest, StampsOutputTimeAndPostsTheFrame) {
  SinkFixture f;
  const Frame frame = f.deliver(0, milliseconds(96));
  EXPECT_EQ(frame.meta.t_out, kT0);
  const std::optional<PreviewItem> posted = f.mailbox.take();
  ASSERT_TRUE(posted.has_value());
  EXPECT_EQ(posted->image.data, frame.image.data);  // shared pixels: the worker copies nothing
  ASSERT_TRUE(posted->info.age_ms.has_value());
  EXPECT_DOUBLE_EQ(*posted->info.age_ms, 96.0);
  EXPECT_EQ(posted->info.policy, "adaptive");
}

TEST(PreviewSinkTest, EveryFrameIsPostedAndOnlyTheNewestKept) {
  SinkFixture f;
  for (std::uint64_t id = 0; id < 5; ++id) f.deliver(id, milliseconds(10 * (id + 1)));
  const std::optional<PreviewItem> posted = f.mailbox.take();
  ASSERT_TRUE(posted.has_value());
  EXPECT_DOUBLE_EQ(*posted->info.age_ms, 50.0);  // frame 4
}

TEST(PreviewSinkTest, DropsAreEstimatedFromIdGaps) {
  SinkFixture f;
  f.deliver(0);
  f.deliver(1);
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

TEST(PreviewSinkTest, ShedddingStateTravelsWithTheFrame) {
  SinkFixture f;
  f.deliver(0);
  EXPECT_FALSE(f.mailbox.take()->info.shedding);
  f.shedding = true;
  f.deliver(1);
  EXPECT_TRUE(f.mailbox.take()->info.shedding);
}

// ---------------------------------------------------------------- display (main thread)

class FakeWindow final : public IPreviewWindow {
 public:
  void show(const cv::Mat& image) override {
    ++shows;
    channels = image.channels();
    last_shown = image.clone();
  }
  int poll_key() override {
    if (keys.empty()) return -1;
    const int key = keys.front();
    keys.pop_front();
    return key;
  }
  bool closed() const override { return is_closed; }

  int shows = 0;
  int channels = 0;
  cv::Mat last_shown;
  std::deque<int> keys;
  bool is_closed = false;
};

struct DisplayFixture {
  FakeWindow window;
  PreviewMailbox mailbox;
  int stops = 0;
  PreviewDisplay display{window, mailbox, [this] { ++stops; }};

  void post(double level, int channels = 3) {
    PreviewItem it;
    // 320x240: big enough that the bottom-right corner is outside the info panel.
    it.image = cv::Mat(240, 320, channels == 3 ? CV_8UC3 : CV_8UC1, cv::Scalar::all(level));
    it.info.panel = false;  // the display decides panel/paused, not the posted info
    mailbox.post(std::move(it));
  }
};

TEST(PreviewDisplayTest, ShowsTheNewestPostedFrameOnly) {
  DisplayFixture f;
  f.display.pump();
  EXPECT_EQ(f.window.shows, 0);  // nothing posted yet
  f.post(10);
  f.post(200);
  f.display.pump();
  EXPECT_EQ(f.window.shows, 1);
  EXPECT_EQ(f.window.last_shown.at<cv::Vec3b>(239, 319), cv::Vec3b(200, 200, 200));
  f.display.pump();
  EXPECT_EQ(f.window.shows, 1);  // no new frame: nothing redrawn
}

TEST(PreviewDisplayTest, QuitKeysRequestOneGracefulStop) {
  for (int key : {static_cast<int>('q'), 27}) {
    DisplayFixture f;
    f.window.keys = {key, key};
    f.display.pump();
    f.display.pump();
    EXPECT_EQ(f.stops, 1) << key;  // asked once, like the first Ctrl+C
  }
}

TEST(PreviewDisplayTest, ClosingTheWindowRequestsOneStop) {
  DisplayFixture f;
  f.window.is_closed = true;
  f.display.pump();
  f.display.pump();
  EXPECT_EQ(f.stops, 1);
}

TEST(PreviewDisplayTest, SpaceFreezesTheShownImage) {
  DisplayFixture f;
  f.post(10);
  f.window.keys = {' '};
  f.display.pump();  // shows 10, then Space: paused, redrawn with the PAUSED tag
  EXPECT_TRUE(f.display.paused());
  EXPECT_EQ(f.window.shows, 2);
  f.post(200);
  f.display.pump();
  EXPECT_EQ(f.window.shows, 2);  // frames keep arriving but are not shown
  f.window.keys = {' '};
  f.display.pump();  // unfreeze (redraw of the frozen image)
  f.post(200);
  f.display.pump();
  EXPECT_EQ(f.window.shows, 4);
  EXPECT_EQ(f.window.last_shown.at<cv::Vec3b>(239, 319), cv::Vec3b(200, 200, 200));
}

TEST(PreviewDisplayTest, OKeyTogglesThePanel) {
  DisplayFixture f;
  f.post(255);
  f.display.pump();
  EXPECT_TRUE(f.display.panel());
  EXPECT_NE(f.window.last_shown.at<cv::Vec3b>(10, 10), cv::Vec3b(255, 255, 255));  // panel
  f.window.keys = {'o'};
  f.display.pump();  // redrawn without the panel
  EXPECT_FALSE(f.display.panel());
  EXPECT_EQ(f.window.last_shown.at<cv::Vec3b>(10, 10), cv::Vec3b(255, 255, 255));
}

TEST(PreviewDisplayTest, GreyFramesAreShownInColour) {
  DisplayFixture f;
  f.post(5, 1);
  f.display.pump();
  EXPECT_EQ(f.window.channels, 3);
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
