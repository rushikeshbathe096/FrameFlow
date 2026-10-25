#include "frameflow/source/video_file_source.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

namespace frameflow {

namespace {

void check_readable(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    throw InputError("cannot open input: " + path + ": no such file", "check the path");
  }
  if (!std::filesystem::is_regular_file(path, ec) || !std::ifstream(path).good()) {
    throw InputError("cannot open input: " + path + ": not a readable file",
                     "check the path and its permissions");
  }
}

}  // namespace

VideoFileSource::VideoFileSource(const std::string& path, bool loop) : path_(path), loop_(loop) {
  check_readable(path_);
  try {
    capture_.open(path_, cv::CAP_FFMPEG);  // D50: one known decoder, no GStreamer fallback
  } catch (const cv::Exception&) {
    capture_.release();  // treated like any other file OpenCV cannot open, below
  }
  if (!capture_.isOpened() || !read(first_)) {
    throw InputError("cannot decode input: " + path_ + ": no decodable video frames",
                     "try an .mp4 with H.264");
  }
}

FramePtr VideoFileSource::next(std::uint64_t id) {
  cv::Mat image;
  if (!first_used_) {
    first_used_ = true;
    image = first_;  // shares pixels with first_, which is never written again
  } else if (!read(image) && !(before_end() && retry(image)) &&
             !(loop_ && rewind() && read(image))) {
    return nullptr;  // end of input (EC-05), or a file that cannot be read again after a rewind
  }
  return std::make_unique<Frame>(id, std::move(image));
}

bool VideoFileSource::before_end() {
  try {
    const double count = capture_.get(cv::CAP_PROP_FRAME_COUNT);
    return count > 0 && capture_.get(cv::CAP_PROP_POS_FRAMES) < count;
  } catch (const cv::Exception&) {
    return false;
  }
}

bool VideoFileSource::retry(cv::Mat& image) {
  for (int attempt = 0; attempt < kDecodeRetries; ++attempt) {
    if (read(image)) return true;  // the bad frame is skipped; ids stay continuous
  }
  throw InputError("cannot decode input: " + path_ + ": " + std::to_string(kDecodeRetries + 1) +
                       " frames in a row failed to decode",
                   "the file may be corrupted or truncated");
}

bool VideoFileSource::read(cv::Mat& image) {
  try {
    return capture_.read(image) && !image.empty();
  } catch (const cv::Exception&) {
    return false;
  }
}

// Reopens the file: seeking to frame 0 is not reliable for every codec.
bool VideoFileSource::rewind() {
  try {
    capture_.release();
    return capture_.open(path_, cv::CAP_FFMPEG);
  } catch (const cv::Exception&) {
    return false;
  }
}

}  // namespace frameflow
