#include "frameflow/pipeline.hpp"

#include <chrono>
#include <exception>
#include <opencv2/core/utility.hpp>
#include <stdexcept>
#include <utility>

#include "frameflow/policy/policy_factory.hpp"
#include "frameflow/source/source_runner.hpp"
#include "frameflow/worker.hpp"

namespace frameflow {

namespace {

const PipelineConfig& validated(const PipelineConfig& config) {
  if (config.duration.has_value() == config.frames.has_value()) {
    throw std::invalid_argument("Pipeline: set exactly one of duration or frames");
  }
  if (!(config.fps > 0.0)) throw std::invalid_argument("Pipeline: fps must be positive");
  if (config.duration && *config.duration <= Clock::duration::zero()) {
    throw std::invalid_argument("Pipeline: duration must be positive");
  }
  if (config.frames && *config.frames == 0) {
    throw std::invalid_argument("Pipeline: frames must be at least 1");
  }
  return config;
}

std::size_t frame_capacity(const PipelineConfig& config) {
  if (config.frames) return static_cast<std::size_t>(*config.frames);
  return frame_capacity_for(
      config.fps,
      std::chrono::duration<double>(config.duration.value_or(Clock::duration::zero())).count());
}

std::string describe(const char* thread_name, std::exception_ptr error) {
  try {
    std::rethrow_exception(std::move(error));
  } catch (const std::exception& e) {
    return std::string(thread_name) + ": " + e.what();
  } catch (...) {
    return std::string(thread_name) + ": unknown exception";
  }
}

}  // namespace

Pipeline::Pipeline(const PipelineConfig& config, Clock& clock, std::unique_ptr<ISource> source,
                   std::unique_ptr<ISink> sink)
    : config_(validated(config)),
      clock_(clock),
      source_(std::move(source)),
      sink_(std::move(sink)),
      metrics_(frame_capacity(config_)),
      policy_(make_policy(config_.policy, clock, config_.adaptive)),
      queue_(
          config_.queue_capacity, *policy_,
          [this](std::size_t depth) { metrics_.record_depth(clock_.now(), depth); },
          [this](FramePtr frame) { record_drop(std::move(frame)); }),
      processor_(make_processor(config_.processor, config_.kernel)),
      cost_(clock, config_.cost, config_.cost_wait, config_.burst) {
  if (!source_ || !sink_) throw std::invalid_argument("Pipeline: source and sink are required");
  // D50: processing runs on the worker thread only. OpenCV's own thread pool would add hidden
  // threads, make latency depend on the core count, and is invisible to TSan.
  cv::setNumThreads(1);
}

Pipeline::~Pipeline() {
  shutdown();
  join_threads();
}

RunResult Pipeline::run() {
  if (started_) throw std::logic_error("Pipeline::run called twice");
  started_ = true;
  t0_ = clock_.now();
  cost_.start(t0_);  // spike schedule is measured from the run start (FR-PROC-04)

  source_thread_ = std::thread([this] { source_main(); });
  try {
    worker_thread_ = std::thread([this] { worker_main(); });
  } catch (...) {
    shutdown();
    join_threads();
    throw;
  }

  bool duration_reached = false;
  const bool stop_requested = wait_for_end(duration_reached);
  const Clock::time_point t_end = clock_.now();

  shutdown();
  join_threads();
  record_in_flight();

  RunResult result;
  result.produced = produced_;
  result.t0 = t0_;
  result.t_end = t_end;
  if (policy_->name() == "adaptive") result.state_changes = policy_->state_changes();
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!error_.empty()) {
    result.ended_by = EndedBy::Error;
    result.error = error_;
  } else if (duration_reached) {
    result.ended_by = EndedBy::Duration;
  } else if (stop_requested) {
    result.ended_by = EndedBy::StopRequested;
  } else if (config_.frames && produced_ == *config_.frames) {
    result.ended_by = EndedBy::Frames;
  } else {
    result.ended_by = EndedBy::InputEnd;
  }
  return result;
}

// Waits for any end condition; with a tick callback, wakes about once per second to call it
// (outside the state lock). Returns whether a stop was requested.
bool Pipeline::wait_for_end(bool& duration_reached) {
  std::unique_lock<std::mutex> lock(state_mutex_);
  const auto ended = [this] { return source_done_ || stop_requested_ || !error_.empty(); };
  while (true) {
    std::optional<Clock::duration> step;
    if (config_.duration) step = *config_.duration - (clock_.now() - t0_);
    if (tick_) step = step ? std::min(*step, tick_interval_) : tick_interval_;
    if (!step) {
      state_changed_.wait(lock, ended);
      break;
    }
    if (state_changed_.wait_for(lock, std::max(*step, Clock::duration::zero()), ended)) break;
    if (config_.duration && clock_.now() - t0_ >= *config_.duration) {
      duration_reached = true;
      break;
    }
    lock.unlock();
    tick_();  // only reached with a tick callback: without one, the wait above ends the run
    lock.lock();
  }
  return stop_requested_;
}

Pipeline::LiveStatus Pipeline::live_status() const {
  LiveStatus status;
  status.elapsed = clock_.now() - t0_;
  status.counts = metrics_.live();
  status.shedding = policy_->shedding();
  return status;
}

void Pipeline::request_stop() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  stop_requested_ = true;
  state_changed_.notify_all();
}

void Pipeline::source_main() {
  SourceRunner runner(*source_, clock_, queue_, metrics_, config_.fps, metrics_.frame_capacity());
  try {
    runner.run(t0_, stop_);
  } catch (...) {
    report_error(describe("source", std::current_exception()));
  }
  produced_ = runner.produced();
  std::lock_guard<std::mutex> lock(state_mutex_);
  source_done_ = true;
  state_changed_.notify_all();
}

void Pipeline::worker_main() {
  try {
    Worker(queue_, *processor_, cost_, *sink_, clock_, metrics_, *policy_)
        .run();  // returns once the queue shuts down
  } catch (...) {
    report_error(describe("worker", std::current_exception()));
  }
}

void Pipeline::report_error(const std::string& message) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (error_.empty()) error_ = message;  // keep the first error
  state_changed_.notify_all();
}

void Pipeline::shutdown() {
  stop_ = true;
  queue_.shutdown();
}

void Pipeline::join_threads() {
  if (source_thread_.joinable()) source_thread_.join();
  if (worker_thread_.joinable()) worker_thread_.join();
}

void Pipeline::record_drop(FramePtr frame) {
  frame->meta.t_drop = clock_.now();
  metrics_.record_frame(frame->meta);
}  // the pixel buffer is freed here

void Pipeline::record_in_flight() {
  for (FramePtr& frame : queue_.drain()) metrics_.record_frame(frame->meta);
}

}  // namespace frameflow
