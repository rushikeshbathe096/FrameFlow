// video_file_source.hpp
// Responsibility: reads frames from a video file with OpenCV (FR-SRC-01). The constructor opens
// the file and decodes the first frame, so a missing, unreadable or undecodable file fails before
// any thread starts (VR-14, EC-06: exit 3). At the end of the file it starts again from the first
// frame (FR-SRC-06, EC-04), or reports the end of input with `loop == false` (EC-05). Frames keep
// the file's own size; their pacing comes from SourceRunner (`--fps`), not from the file (D48).
// Input files are untrusted: OpenCV errors become InputError exceptions, never crashes (SR-01).
// A frame that fails to decode before the end of the file is retried with the next frames up to
// 3 times (ER-03, EC-07): the first success continues the run (the bad frame is skipped), three
// failures throw InputError, which ends the run with an error (exit 1, valid=false) (D49).
// Thread-safety: none; constructed on the main thread, then used by the source thread only.
#pragma once

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <stdexcept>
#include <string>

#include "frameflow/source/source.hpp"

namespace frameflow {

// A problem with the input file; `what()` is an ER-01 style "what: path: detail" message.
class InputError : public std::runtime_error {
 public:
  InputError(const std::string& message, std::string hint)
      : std::runtime_error(message), hint_(std::move(hint)) {}
  const std::string& hint() const { return hint_; }

 private:
  std::string hint_;
};

class VideoFileSource final : public ISource {
 public:
  VideoFileSource(const std::string& path, bool loop);  // throws InputError

  static constexpr int kDecodeRetries = 3;  // ER-03

  // nullptr at the end of input when not looping; throws InputError after kDecodeRetries failed
  // decodes in a row before the end of the file.
  FramePtr next(std::uint64_t id) override;

  cv::Size frame_size() const { return first_.size(); }

 private:
  bool read(cv::Mat& image);   // false at the end of the file or on a decode failure
  bool before_end();           // true while the position is before the reported frame count
  bool retry(cv::Mat& image);  // ER-03: up to kDecodeRetries more reads; throws if all fail
  bool rewind();

  std::string path_;
  bool loop_;
  cv::VideoCapture capture_;
  cv::Mat first_;  // decoded in the constructor (VR-14); handed out by the first next()
  bool first_used_ = false;
};

}  // namespace frameflow
