// worker.hpp
// Responsibility: the worker thread's loop. Pops frames (100 ms timeout, FR-Q-05), stamps t_pop,
// t_proc_start and t_proc_end around the processing cost (processor `none` in Phase 1), passes
// each frame to the sink, which stamps t_out (FR-PROC-05, FR-PROC-06), and records it in the
// MetricsStore. Returns when the queue is shut down (EC-10).
// Thread-safety: run() is called by one thread (the worker thread).
#pragma once

#include <chrono>

#include "frameflow/bounded_queue.hpp"
#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"
#include "frameflow/processor/cost_model.hpp"
#include "frameflow/sink/sink.hpp"

namespace frameflow {

class Worker {
 public:
  static constexpr std::chrono::milliseconds kPopTimeout{100};

  Worker(BoundedQueue<FramePtr>& queue, const CostModel& cost, ISink& sink, Clock& clock,
         MetricsStore& metrics);

  // Processes frames until the queue is shut down. If processing a frame throws, that frame is
  // recorded as in-flight (no t_out) and the exception is rethrown to the caller.
  void run();

 private:
  void process(Frame& frame);

  BoundedQueue<FramePtr>& queue_;
  const CostModel& cost_;
  ISink& sink_;
  Clock& clock_;
  MetricsStore& metrics_;
};

}  // namespace frameflow
