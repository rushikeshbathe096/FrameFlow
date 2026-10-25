// source_runner.hpp
// Responsibility: the source thread's loop. Asks the ISource for frame k (ids 0, 1, 2... with no
// gaps, FR-SRC-05), waits until its scheduled release t0 + k / fps (FR-SRC-03), stamps t_cap with
// the scheduled time even when the release is late (FR-SRC-04), records the lateness (FR-SRC-07),
// stamps t_push (D13) and pushes the frame into the queue, where the policy may drop it.
// Thread-safety: run() is called by one thread (the source thread). It only reads `stop`, which
// other threads may set at any time.
#pragma once

#include <atomic>
#include <cstdint>

#include "frameflow/bounded_queue.hpp"
#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"
#include "frameflow/source/source.hpp"

namespace frameflow {

class SourceRunner {
 public:
  SourceRunner(ISource& source, Clock& clock, BoundedQueue<FramePtr>& queue, MetricsStore& metrics,
               double fps, std::uint64_t max_frames);

  // Releases frames until max_frames have been produced, `stop` becomes true, the source ends or
  // the queue is shut down. A frame whose push fails because of shutdown is recorded as
  // in-flight (no t_push). Returns the number of frames produced (released with a t_cap).
  std::uint64_t run(Clock::time_point t0, const std::atomic<bool>& stop);

  // Frames produced so far; still correct if run() exited with an exception. Read it from the
  // source thread, or after that thread has joined.
  std::uint64_t produced() const { return produced_; }

  // Scheduled release time of frame k.
  Clock::time_point release_time(Clock::time_point t0, std::uint64_t k) const;

 private:
  // Sleeps until t in steps of at most 50 ms, returning early once `stop` is set.
  void wait_until(Clock::time_point t, const std::atomic<bool>& stop);

  ISource& source_;
  Clock& clock_;
  BoundedQueue<FramePtr>& queue_;
  MetricsStore& metrics_;
  double fps_;
  std::uint64_t max_frames_;
  std::uint64_t produced_ = 0;
};

}  // namespace frameflow
