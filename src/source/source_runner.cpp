#include "frameflow/source/source_runner.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace frameflow {

namespace {
constexpr std::chrono::milliseconds kStopCheckInterval{50};
}  // namespace

Clock::duration BackpressureTracker::on_release(Clock::duration lateness) {
  pending_ =
      std::clamp(pending_, Clock::duration::zero(), std::max(lateness, Clock::duration::zero()));
  return pending_;
}

SourceRunner::SourceRunner(ISource& source, Clock& clock, BoundedQueue<FramePtr>& queue,
                           MetricsStore& metrics, double fps, std::uint64_t max_frames)
    : source_(source),
      clock_(clock),
      queue_(queue),
      metrics_(metrics),
      fps_(fps),
      max_frames_(max_frames) {
  if (!(fps > 0.0)) throw std::invalid_argument("SourceRunner: fps must be positive");
}

Clock::time_point SourceRunner::release_time(Clock::time_point t0, std::uint64_t k) const {
  const std::chrono::duration<double> offset(static_cast<double>(k) / fps_);
  return t0 + std::chrono::duration_cast<Clock::duration>(offset);
}

void SourceRunner::wait_until(Clock::time_point t, const std::atomic<bool>& stop) {
  while (!stop) {
    const Clock::time_point now = clock_.now();
    if (now >= t) return;
    clock_.sleep_until(std::min(t, now + kStopCheckInterval));
  }
}

std::uint64_t SourceRunner::run(Clock::time_point t0, const std::atomic<bool>& stop) {
  produced_ = 0;
  BackpressureTracker backpressure;
  for (std::uint64_t k = 0; k < max_frames_ && !stop; ++k) {
    FramePtr frame = source_.next(k);  // decode/generate before the release time
    if (!frame) break;                 // end of input

    const Clock::time_point scheduled = release_time(t0, k);
    wait_until(scheduled, stop);
    if (stop) break;  // never released: not produced

    const Clock::time_point released = clock_.now();
    frame->meta.t_cap = scheduled;
    frame->meta.source_lateness = released - scheduled;
    frame->meta.backpressure = backpressure.on_release(frame->meta.source_lateness);
    frame->meta.t_push = released;
    ++produced_;

    // Queued: the frame now belongs to the worker. Dropped: the queue's drop callback recorded
    // it. Only on Shutdown is the frame still ours.
    const PushResult pushed = queue_.push(std::move(frame));
    backpressure.on_push_wait(clock_.now() - released);  // long only while blocked on a full queue
    if (pushed == PushResult::Shutdown) {
      // Shutdown while waiting for space: the frame is still owned here and is in-flight.
      // NOLINTNEXTLINE(bugprone-use-after-move): a failed push leaves `frame` untouched
      frame->meta.t_push.reset();
      metrics_.record_frame(frame->meta);
      break;
    }
  }
  return produced_;
}

}  // namespace frameflow
