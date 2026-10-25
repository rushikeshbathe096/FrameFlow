// worker.hpp
// Responsibility: the worker thread's loop. Pops frames (100 ms timeout, FR-Q-05) and stamps
// t_pop. If the policy says so (adaptive while SHEDDING), drops a stale frame instead of
// processing it (reason `stale`, t_drop, FR-POL-08/10). Otherwise stamps t_proc_start and
// t_proc_end around the processor (FR-PROC-01) and then the synthetic cost, passes the frame to the
// sink, which stamps t_out (FR-PROC-05, FR-PROC-06), feeds the delivered latency back to the policy
// (FR-POL-05) and records the frame in the MetricsStore. Returns when the queue is shut down
// (EC-10). Thread-safety: run() is called by one thread (the worker thread). The policy's
// worker-side methods are called without the queue lock (see overload_policy.hpp).
#pragma once

#include <chrono>

#include "frameflow/bounded_queue.hpp"
#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"
#include "frameflow/policy/overload_policy.hpp"
#include "frameflow/processor/cost_model.hpp"
#include "frameflow/processor/processor.hpp"
#include "frameflow/sink/sink.hpp"

namespace frameflow {

class Worker {
 public:
  static constexpr std::chrono::milliseconds kPopTimeout{100};

  Worker(BoundedQueue<FramePtr>& queue, const IProcessor& processor, const CostModel& cost,
         ISink& sink, Clock& clock, MetricsStore& metrics, OverloadPolicy& policy);

  // Processes frames until the queue is shut down. If processing a frame throws, that frame is
  // recorded as in-flight (no t_out) and the exception is rethrown to the caller.
  void run();

 private:
  bool drop_if_stale(Frame& frame, Clock::time_point popped);  // true: dropped and recorded
  void process(Frame& frame);

  BoundedQueue<FramePtr>& queue_;
  const IProcessor& processor_;
  const CostModel& cost_;
  ISink& sink_;
  Clock& clock_;
  MetricsStore& metrics_;
  OverloadPolicy& policy_;
};

}  // namespace frameflow
