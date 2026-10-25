#include "frameflow/worker.hpp"

#include <optional>

namespace frameflow {

Worker::Worker(BoundedQueue<FramePtr>& queue, const CostModel& cost, ISink& sink, Clock& clock,
               MetricsStore& metrics)
    : queue_(queue), cost_(cost), sink_(sink), clock_(clock), metrics_(metrics) {}

void Worker::run() {
  while (true) {
    std::optional<FramePtr> frame = queue_.pop(kPopTimeout);
    if (!frame) {
      if (queue_.is_shutdown()) return;
      continue;  // timeout: keep waiting
    }
    try {
      process(**frame);
    } catch (...) {
      (*frame)->meta.t_out.reset();  // not delivered: counts as in-flight (BR-02)
      metrics_.record_frame((*frame)->meta);
      throw;
    }
    metrics_.record_frame((*frame)->meta);
  }
}

void Worker::process(Frame& frame) {
  frame.meta.t_pop = clock_.now();
  frame.meta.t_proc_start = clock_.now();
  cost_.apply();  // processor `none`: the synthetic cost is the only work
  frame.meta.t_proc_end = clock_.now();
  sink_.consume(frame);
}

}  // namespace frameflow
