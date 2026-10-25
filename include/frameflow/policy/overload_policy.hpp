// overload_policy.hpp
// Responsibility: strategy interface that decides what a push does (FR-Q-04): insert, wait for
// space, or drop frames; and, for the adaptive policy, whether the worker drops a popped frame
// and how delivered latency feeds back (SPEC 9.2). The queue owns no policy logic; it applies
// the returned decision and records the drop reason on each dropped frame.
// Thread-safety: on_push() and is_stale() are called by the queue with its mutex held (source
// thread), so those calls never overlap each other. should_drop_on_pop() and on_delivered() are
// called by the worker thread without the queue lock, possibly at the same time as on_push(); an
// implementation with state shared between them must guard it (AdaptivePolicy uses a mutex).
// No method may call back into the queue. state_changes() is read after the threads have joined.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"

namespace frameflow {

enum class PushDecision {
  Insert,        // append the incoming frame (only valid when there is space)
  Wait,          // block until there is space, then ask again
  DropOldest,    // full: remove the head (reason `oldest`), then append the incoming frame
  DropIncoming,  // full: discard the incoming frame (reason `newest`); queue unchanged
  DropStale,     // remove every queued frame is_stale() accepts (reason `stale`); if still full,
                 // remove the head (reason `oldest`); then append the incoming frame (FR-POL-08)
};

// Read-only snapshot of the queue passed to a policy.
struct QueueView {
  std::size_t size = 0;
  std::size_t capacity = 0;
  bool full() const { return size >= capacity; }
};

// Adaptive policy states (FR-POL-06) and the record of one change (FR-POL-09).
enum class AdaptiveState { Normal, Shedding };

constexpr std::string_view state_name(AdaptiveState state) {
  return state == AdaptiveState::Normal ? "NORMAL" : "SHEDDING";
}

struct StateChange {
  Clock::time_point t;
  AdaptiveState from = AdaptiveState::Normal;
  AdaptiveState to = AdaptiveState::Normal;
  Clock::duration latency_estimate{0};  // L when the change happened
};

class OverloadPolicy {
 public:
  virtual ~OverloadPolicy() = default;

  // Called for every push, with the queue lock held.
  virtual PushDecision on_push(const QueueView& queue, const FrameMeta& incoming) = 0;

  // For PushDecision::DropStale: asked for each queued frame, with the queue lock held.
  virtual bool is_stale(const FrameMeta& /*queued*/) const { return false; }

  // Worker, right after a pop: true drops the frame (reason `stale`) instead of processing it.
  virtual bool should_drop_on_pop(const FrameMeta& /*frame*/, Clock::time_point /*now*/) {
    return false;
  }

  // Worker, after the sink: the delivered frame's end-to-end latency and its processing time
  // (t_proc_end - t_proc_start), at time `now` (D44, D47).
  virtual void on_delivered(Clock::duration /*latency*/, Clock::duration /*processing*/,
                            Clock::time_point /*now*/) {}

  // True while the adaptive policy is SHEDDING (the preview's amber border); any thread.
  virtual bool shedding() const { return false; }

  // Every state change so far (adaptive only); read after the pipeline threads have joined.
  virtual std::vector<StateChange> state_changes() const { return {}; }

  virtual std::string_view name() const = 0;
};

}  // namespace frameflow
