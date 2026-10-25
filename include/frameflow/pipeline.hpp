// pipeline.hpp
// Responsibility: owns and wires one run: source -> bounded queue -> worker -> sink, plus the
// MetricsStore. run() starts the source and worker threads, waits for the end condition (duration,
// frame count, input end, stop request or a thread error), then stops everything through one
// shutdown routine (ER-08), joins the threads and records frames still queued as in-flight
// (BR-01, BR-02). An exception on either thread stops the whole pipeline (ER-04).
// Thread-safety: run() is called once, by one thread (normally main). request_stop() may be called
// from any thread at any time. metrics() may be read only after run() has returned.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "frameflow/bounded_queue.hpp"
#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"
#include "frameflow/policy/overload_policy.hpp"
#include "frameflow/processor/cost_model.hpp"
#include "frameflow/sink/sink.hpp"
#include "frameflow/source/source.hpp"

namespace frameflow {

struct PipelineConfig {
  double fps = 30.0;
  std::size_t queue_capacity = 8;
  Clock::duration cost{0};
  CostWait cost_wait = CostWait::Busy;
  std::string policy = "blocking";  // one policy per run, fixed for its whole duration (BR-08)
  // Exactly one of these must be set (FR-CLI-05).
  std::optional<Clock::duration> duration;
  std::optional<std::uint64_t> frames;
};

enum class EndedBy { Duration, Frames, InputEnd, StopRequested, Error };

struct RunResult {
  EndedBy ended_by = EndedBy::Error;
  std::uint64_t produced = 0;  // frames released by the source (each has a record)
  Clock::time_point t0;        // run start: scheduled release of frame 0
  Clock::time_point t_end;     // when the end condition was reached (shutdown started)
  std::string error;           // "<thread>: <message>" when ended_by == Error
};

class Pipeline {
 public:
  // Throws std::invalid_argument for an invalid config (not exactly one of duration/frames,
  // fps <= 0, queue capacity 0, unknown policy).
  Pipeline(const PipelineConfig& config, Clock& clock, std::unique_ptr<ISource> source,
           std::unique_ptr<ISink> sink);
  ~Pipeline();  // if threads are still running: shutdown and join (RAII)

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  RunResult run();  // may be called once; throws std::logic_error on a second call
  void request_stop();
  const MetricsStore& metrics() const { return metrics_; }

 private:
  void source_main();
  void worker_main();
  void report_error(const std::string& message);
  void shutdown();  // the single shutdown routine: stop flag + queue shutdown
  void join_threads();
  void record_in_flight();
  void record_drop(FramePtr frame);  // queue drop callback: runs under the queue lock

  const PipelineConfig config_;
  Clock& clock_;
  std::unique_ptr<ISource> source_;
  std::unique_ptr<ISink> sink_;
  MetricsStore metrics_;  // declared before queue_: the queue's depth callback writes into it
  const std::unique_ptr<OverloadPolicy> policy_;  // before queue_: the queue references it
  BoundedQueue<FramePtr> queue_;
  const CostModel cost_;

  std::atomic<bool> stop_{false};  // read by the source loop
  Clock::time_point t0_;
  std::uint64_t produced_ = 0;  // written by the source thread, read after join

  std::mutex state_mutex_;  // guards the three fields below
  std::condition_variable state_changed_;
  bool source_done_ = false;
  bool stop_requested_ = false;
  std::string error_;

  bool started_ = false;
  std::thread source_thread_;
  std::thread worker_thread_;
};

}  // namespace frameflow
