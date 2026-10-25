// overload_policy.hpp
// Responsibility: strategy interface that decides what a push does (FR-Q-04): insert, wait for
// space, or drop a frame. The queue owns no policy logic; it applies the returned decision and
// records the drop reason on the dropped frame.
// Thread-safety: on_push() is called by the queue with its mutex held, so calls never overlap.
// Implementations must not call back into the queue.
#pragma once

#include <cstddef>
#include <string_view>

#include "frameflow/frame.hpp"

namespace frameflow {

enum class PushDecision {
  Insert,        // append the incoming frame (only valid when there is space)
  Wait,          // block until there is space, then ask again
  DropOldest,    // full: remove the head (reason `oldest`), then append the incoming frame
  DropIncoming,  // full: discard the incoming frame (reason `newest`); queue unchanged
};

// Read-only snapshot of the queue passed to a policy.
struct QueueView {
  std::size_t size = 0;
  std::size_t capacity = 0;
  bool full() const { return size >= capacity; }
};

class OverloadPolicy {
 public:
  virtual ~OverloadPolicy() = default;

  // Called for every push, with the queue lock held.
  virtual PushDecision on_push(const QueueView& queue, const FrameMeta& incoming) = 0;

  virtual std::string_view name() const = 0;
};

}  // namespace frameflow
