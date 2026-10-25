// blocking_policy.hpp
// Responsibility: the `blocking` overload policy (FR-POL-02). When the queue is full the producer
// waits until space is free or shutdown is requested; no frame is ever dropped.
// Thread-safety: stateless; safe from any thread.
#pragma once

#include "frameflow/policy/overload_policy.hpp"

namespace frameflow {

class BlockingPolicy final : public OverloadPolicy {
 public:
  PushDecision on_push(const QueueView& queue, const FrameMeta& /*incoming*/) override {
    return queue.full() ? PushDecision::Wait : PushDecision::Insert;
  }
  std::string_view name() const override { return "blocking"; }
};

}  // namespace frameflow
