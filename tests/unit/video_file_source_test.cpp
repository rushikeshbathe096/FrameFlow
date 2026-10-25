#include "frameflow/source/video_file_source.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <opencv2/imgproc.hpp>
#include <random>
#include <string>
#include <vector>

namespace frameflow {
namespace {

namespace fs = std::filesystem;

constexpr int kClipFrames = 10;

// Grey level of clip frame i; far enough apart to survive MJPEG compression.
double level(int i) { return 20.0 * (i + 1); }

class VideoFileSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  // A 64x48 MJPEG clip written by OpenCV itself (no downloaded files).
  std::string write_clip(const std::string& name = "clip.avi", int frames = kClipFrames) {
    const std::string path = (dir_ / name).string();
    cv::VideoWriter writer(path, cv::CAP_OPENCV_MJPEG, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                           30.0, cv::Size(64, 48));
    EXPECT_TRUE(writer.isOpened());
    for (int i = 0; i < frames; ++i) {
      writer.write(cv::Mat(48, 64, CV_8UC3, cv::Scalar::all(level(i % kClipFrames))));
    }
    return path;
  }

  std::string clip_bytes(int frames) {
    std::ifstream in(write_clip("source.avi", frames), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  std::string write_bytes(const std::string& name, const std::string& bytes) {
    const std::string path = (dir_ / name).string();
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
  }

  fs::path dir_ =
      fs::temp_directory_path() / ("frameflow_video_test_" + std::to_string(::getpid()));
};

double mean_level(const FramePtr& frame) { return cv::mean(frame->image)[0]; }

TEST_F(VideoFileSourceTest, ReadsFramesInOrderWithGivenIds) {
  VideoFileSource source(write_clip(), true);
  EXPECT_EQ(source.frame_size(), cv::Size(64, 48));
  for (int i = 0; i < kClipFrames; ++i) {
    const FramePtr frame = source.next(static_cast<std::uint64_t>(i));
    ASSERT_NE(frame, nullptr) << i;
    EXPECT_EQ(frame->meta.id, static_cast<std::uint64_t>(i));
    EXPECT_EQ(frame->image.size(), cv::Size(64, 48));
    EXPECT_NEAR(mean_level(frame), level(i), 6.0) << "frame " << i;
  }
}

// FR-SRC-06, EC-04: a clip shorter than a second loops; the caller's ids continue without gaps.
TEST_F(VideoFileSourceTest, LoopsBackToTheFirstFrame) {
  VideoFileSource source(write_clip(), true);
  for (std::uint64_t id = 0; id < 25; ++id) {
    const FramePtr frame = source.next(id);
    ASSERT_NE(frame, nullptr) << id;
    EXPECT_EQ(frame->meta.id, id);
    EXPECT_NEAR(mean_level(frame), level(static_cast<int>(id % kClipFrames)), 6.0) << id;
  }
}

// EC-05: without looping the input ends after the last frame.
TEST_F(VideoFileSourceTest, NoLoopEndsAfterTheLastFrame) {
  VideoFileSource source(write_clip(), false);
  for (int i = 0; i < kClipFrames; ++i)
    ASSERT_NE(source.next(static_cast<std::uint64_t>(i)), nullptr);
  EXPECT_EQ(source.next(kClipFrames), nullptr);
  EXPECT_EQ(source.next(kClipFrames + 1), nullptr);
}

// Frames are owned by different threads over time (worker, sink), so no two may share pixels.
TEST_F(VideoFileSourceTest, FramesDoNotShareMemory) {
  VideoFileSource source(write_clip(), true);
  std::vector<FramePtr> frames;
  for (std::uint64_t id = 0; id < kClipFrames + 2; ++id) frames.push_back(source.next(id));
  for (std::size_t i = 0; i < frames.size(); ++i) {
    for (std::size_t j = i + 1; j < frames.size(); ++j) {
      EXPECT_NE(frames[i]->image.data, frames[j]->image.data) << i << " " << j;
    }
  }
  frames[0]->image.setTo(cv::Scalar::all(0));         // a processor writing into one frame...
  EXPECT_NEAR(mean_level(frames[1]), level(1), 6.0);  // ...never changes another
}

// Reads up to `limit` frames; returns how many came back before the end or an InputError.
struct ReadOutcome {
  int frames = 0;
  bool ended = false;  // next() returned nullptr
  std::string error;   // InputError message, if one was thrown
};

ReadOutcome read_all(VideoFileSource& source, int limit) {
  ReadOutcome outcome;
  try {
    for (; outcome.frames < limit; ++outcome.frames) {
      if (!source.next(static_cast<std::uint64_t>(outcome.frames))) {
        outcome.ended = true;
        break;
      }
    }
  } catch (const InputError& e) {
    outcome.error = e.what();
  }
  return outcome;
}

// ER-03: a truncated file fails to decode before its reported end; after 3 retries the source
// throws, which ends the run with an error instead of looping or ending quietly.
TEST_F(VideoFileSourceTest, TruncatedFileFailsAfterRetries) {
  const std::string bytes = clip_bytes(30);
  for (bool loop : {true, false}) {
    VideoFileSource source(write_bytes("truncated.avi", bytes.substr(0, bytes.size() * 6 / 10)),
                           loop);
    const ReadOutcome outcome = read_all(source, 100);
    EXPECT_GT(outcome.frames, 5) << "the readable part is delivered first";
    EXPECT_LT(outcome.frames, 30);
    EXPECT_FALSE(outcome.ended);
    EXPECT_NE(outcome.error.find("frames in a row failed to decode"), std::string::npos)
        << outcome.error;
  }
}

// EC-07: a damaged frame in the middle is skipped when the next one decodes.
TEST_F(VideoFileSourceTest, SingleCorruptFrameIsSkipped) {
  std::string bytes = clip_bytes(30);
  const std::size_t middle = bytes.size() / 2;
  for (std::size_t i = middle; i < middle + 200; ++i) bytes[i] = static_cast<char>(i * 37);
  VideoFileSource source(write_bytes("corrupt.avi", bytes), false);
  const ReadOutcome outcome = read_all(source, 100);
  EXPECT_GE(outcome.frames, 25) << outcome.error;  // only the damaged frame(s) are lost
}

// SR-01: damaged files never crash; every outcome is frames, end of input or InputError.
TEST_F(VideoFileSourceTest, FuzzedFilesNeverCrash) {
  const std::string original = clip_bytes(30);
  std::mt19937 random(2024);
  for (int variant = 0; variant < 60; ++variant) {
    std::string bytes = original;
    const int mode = variant % 3;
    if (mode == 0) {  // flip random bytes all over the file
      for (int i = 0; i < 40; ++i) bytes[random() % bytes.size()] = static_cast<char>(random());
    } else if (mode == 1) {  // truncate anywhere
      bytes.resize(random() % bytes.size());
    } else {  // overwrite a random block
      const std::size_t start = random() % bytes.size();
      for (std::size_t i = start; i < std::min(bytes.size(), start + 512); ++i) {
        bytes[i] = static_cast<char>(random());
      }
    }
    const std::string path = write_bytes("fuzz.avi", bytes);
    try {
      VideoFileSource source(path, variant % 2 == 0);
      read_all(source, 80);
    } catch (const InputError&) {
      // not openable or not decodable: the expected way to fail (exit 3)
    }
  }
}

void expect_input_error(const std::string& path, const std::string& part) {
  try {
    VideoFileSource source(path, true);
    FAIL() << "expected InputError for " << path;
  } catch (const InputError& e) {
    EXPECT_NE(std::string(e.what()).find(part), std::string::npos) << e.what();
    EXPECT_NE(std::string(e.what()).find(path), std::string::npos) << e.what();
    EXPECT_FALSE(e.hint().empty());
  }
}

TEST_F(VideoFileSourceTest, MissingFile) {
  expect_input_error((dir_ / "nope.mp4").string(), "cannot open input");
}

// EC-06: zero decodable frames.
TEST_F(VideoFileSourceTest, NotAVideo) {
  std::string noise(4096, '\0');
  std::mt19937 random(7);
  for (char& c : noise) c = static_cast<char>(random());
  expect_input_error(write_bytes("noise.mp4", noise), "cannot decode input");
}

TEST_F(VideoFileSourceTest, EmptyFile) {
  expect_input_error(write_bytes("empty.mp4", ""), "cannot decode input");
}

TEST_F(VideoFileSourceTest, DirectoryIsNotReadable) {
  fs::create_directory(dir_ / "folder.mp4");
  expect_input_error((dir_ / "folder.mp4").string(), "not a readable file");
}

}  // namespace
}  // namespace frameflow
