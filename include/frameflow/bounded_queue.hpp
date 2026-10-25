// bounded_queue.hpp
// Responsibility: fixed-capacity FIFO queue that hands items (FramePtr in the pipeline) from the
// source thread to the worker thread. It never holds more than capacity() items (FR-Q-01),
// delivers them in insertion order (FR-Q-02), lets an OverloadPolicy decide every push (FR-Q-04),
// blocks an empty pop, and wakes every blocked caller on shutdown() (FR-Q-05, FR-Q-06). An
// optional depth callback receives the queue depth after every completed push and every pop
// (FR-Q-07); an optional drop callback receives every frame a policy drops (FR-POL-10).
// T is a pointer-like item whose pointee has a `meta` (FrameMeta) member, e.g. FramePtr (D18).
// Thread-safety: every public member may be called concurrently from any thread (FR-Q-03); one
// mutex guards all state, and both condition variables always wait with a predicate. The policy
// and both callbacks run while that mutex is held, so their calls never overlap and depth
// samples arrive in the order the depth changed; they must be quick and must not call back into
// the queue (the mutex is not recursive). The policy must outlive the queue.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "frameflow/policy/overload_policy.hpp"

namespace frameflow {

enum class PushResult {
  Queued,    // the item is in the queue
  Dropped,   // the policy dropped the incoming item (it went to the drop callback)
  Shutdown,  // the queue is shut down; the item was not consumed
};

template <typename T>
class BoundedQueue {
 public:
  using DepthCallback = std::function<void(std::size_t depth)>;
  using DropCallback = std::function<void(T dropped)>;  // dropped->meta.drop_reason is set

  BoundedQueue(std::size_t capacity, OverloadPolicy& policy, DepthCallback on_depth = {},
               DropCallback on_drop = {})
      : capacity_(capacity),
        policy_(policy),
        on_depth_(std::move(on_depth)),
        on_drop_(std::move(on_drop)) {
    if (capacity == 0) throw std::invalid_argument("BoundedQueue: capacity must be at least 1");
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // Asks the policy what to do and applies it. On Shutdown the item is left untouched; on Queued
  // or Dropped it has been consumed.
  PushResult push(T&& item) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!closed_) {
      const PushDecision decision = policy_.on_push(view(), item->meta);
      const bool full = items_.size() >= capacity_;
      if (decision == PushDecision::DropIncoming && full) {
        drop(std::move(item), DropReason::Newest);
        report_depth();
        return PushResult::Dropped;
      }
      if (decision == PushDecision::DropOldest && full) {
        T oldest = std::move(items_.front());
        items_.pop_front();
        drop(std::move(oldest), DropReason::Oldest);
      }
      if (decision != PushDecision::Wait && items_.size() < capacity_) {
        items_.push_back(std::move(item));
        report_depth();
        not_empty_.notify_one();
        return PushResult::Queued;
      }
      // Wait, or a decision the queue cannot honour (e.g. Insert into a full queue): block until
      // there is space or shutdown, then ask the policy again. Capacity always holds (FR-Q-01).
      not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
    }
    return PushResult::Shutdown;
  }

  // Waits up to timeout for an item. Returns the oldest item, or std::nullopt on timeout or
  // shutdown (items still queued at shutdown are left for drain()).
  std::optional<T> pop(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready =
        not_empty_.wait_for(lock, timeout, [this] { return closed_ || !items_.empty(); });
    if (!ready || closed_) return std::nullopt;
    T item = std::move(items_.front());
    items_.pop_front();
    report_depth();
    not_full_.notify_one();
    return item;
  }

  // Wakes every thread blocked in push() or pop(); afterwards both return immediately.
  void shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  // Removes and returns every queued item. Used after shutdown, once the pipeline threads have
  // joined, to account for frames still in flight (BR-01).
  std::deque<T> drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::deque<T> out;
    out.swap(items_);
    return out;
  }

  bool is_shutdown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

  std::size_t capacity() const { return capacity_; }

 private:
  QueueView view() const { return QueueView{items_.size(), capacity_}; }  // caller holds mutex_

  void report_depth() {  // caller holds mutex_
    if (on_depth_) on_depth_(items_.size());
  }

  void drop(T item, DropReason reason) {  // caller holds mutex_
    item->meta.drop_reason = reason;
    if (on_drop_) on_drop_(std::move(item));
  }

  mutable std::mutex mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::deque<T> items_;
  const std::size_t capacity_;
  OverloadPolicy& policy_;
  const DepthCallback on_depth_;
  const DropCallback on_drop_;
  bool closed_ = false;
};

}  // namespace frameflow
