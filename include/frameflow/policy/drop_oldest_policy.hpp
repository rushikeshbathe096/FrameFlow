// drop_oldest_policy.hpp
// Responsibility: the `drop-oldest` overload policy (FR-POL-03). When the queue is full the frame
// at the head is dropped (reason `oldest`) and the incoming frame is appended, so the queue always
// holds the newest frames. The producer never waits.
// Thread-safety: stateless; safe from any thread.
#pragma once

#include "frameflow/policy/overload_policy.hpp"

namespace frameflow {

class DropOldestPolicy final : public OverloadPolicy {
 public:
  PushDecision on_push(const QueueView& queue, const FrameMeta& /*incoming*/) override {
    return queue.full() ? PushDecision::DropOldest : PushDecision::Insert;
  }
  std::string_view name() const override { return "drop-oldest"; }
};

}  // namespace frameflow
