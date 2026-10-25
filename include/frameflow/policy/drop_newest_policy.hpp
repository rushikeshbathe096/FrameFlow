// drop_newest_policy.hpp
// Responsibility: the `drop-newest` overload policy (FR-POL-04). When the queue is full the
// incoming frame is dropped (reason `newest`) and the queue is left unchanged. The producer never
// waits.
// Thread-safety: stateless; safe from any thread.
#pragma once

#include "frameflow/policy/overload_policy.hpp"

namespace frameflow {

class DropNewestPolicy final : public OverloadPolicy {
 public:
  PushDecision on_push(const QueueView& queue, const FrameMeta& /*incoming*/) override {
    return queue.full() ? PushDecision::DropIncoming : PushDecision::Insert;
  }
  std::string_view name() const override { return "drop-newest"; }
};

}  // namespace frameflow
