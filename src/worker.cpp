#include "frameflow/worker.hpp"

#include <optional>

namespace frameflow {

Worker::Worker(BoundedQueue<FramePtr>& queue, const IProcessor& processor, const CostModel& cost,
               ISink& sink, Clock& clock, MetricsStore& metrics, OverloadPolicy& policy)
    : queue_(queue),
      processor_(processor),
      cost_(cost),
      sink_(sink),
      clock_(clock),
      metrics_(metrics),
      policy_(policy) {}

void Worker::run() {
  while (true) {
    std::optional<FramePtr> frame = queue_.pop(kPopTimeout);
    if (!frame) {
      if (queue_.is_shutdown()) return;
      continue;  // timeout: keep waiting
    }
    const Clock::time_point popped = clock_.now();
    (*frame)->meta.t_pop = popped;
    if (drop_if_stale(**frame, popped)) continue;
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

bool Worker::drop_if_stale(Frame& frame, Clock::time_point popped) {
  if (!policy_.should_drop_on_pop(frame.meta, popped)) return false;
  frame.meta.drop_reason = DropReason::Stale;
  frame.meta.t_drop = popped;
  metrics_.record_frame(frame.meta);
  return true;
}

void Worker::process(Frame& frame) {
  frame.meta.t_proc_start = clock_.now();
  processor_.process(frame.image);
  cost_.apply();  // synthetic cost on top (0 by default)
  frame.meta.t_proc_end = clock_.now();
  sink_.consume(frame);
  if (frame.meta.t_out && frame.meta.t_cap) {
    policy_.on_delivered(*frame.meta.t_out - *frame.meta.t_cap,
                         *frame.meta.t_proc_end - *frame.meta.t_proc_start, *frame.meta.t_out);
  }
}

}  // namespace frameflow
