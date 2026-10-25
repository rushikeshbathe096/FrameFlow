// bench_queue.cpp: `frameflow_bench_queue`, the PR-03 and PR-04 micro-benchmark tool.
// Responsibility: measures (1) one uncontended push+pop pair on BoundedQueue<FramePtr> with the
// blocking policy and no callbacks (PR-03), and (2) the per-frame metrics work the pipeline adds:
// one MetricsStore::record_frame plus two record_depth calls (one push, one pop), for the PR-04
// analysis (that cost divided by the per-frame processing time must stay under 1%, D56). Each
// sample times `pairs` operations; the tool prints the median, min and max per operation over
// `samples` samples. Build with the release preset; numbers are recorded only when measured.
// Thread-safety: single-threaded program.
//
// Usage: frameflow_bench_queue [samples] [pairs]   (defaults: 101 samples of 10000 pairs)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "frameflow/bounded_queue.hpp"
#include "frameflow/clock.hpp"
#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"
#include "frameflow/policy/blocking_policy.hpp"

namespace {

using frameflow::FramePtr;

std::size_t parse_count(const char* text, std::size_t fallback) {
  if (text == nullptr) return fallback;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || value == 0) {
    throw std::invalid_argument(std::string("'") + text + "' is not a positive count");
  }
  return static_cast<std::size_t>(value);
}

// Nanoseconds per pair for one sample. The frame is moved in and out, never reallocated.
double time_pairs(frameflow::BoundedQueue<FramePtr>& queue, FramePtr& frame, std::size_t pairs,
                  frameflow::Clock& clock) {
  const frameflow::Clock::time_point start = clock.now();
  for (std::size_t i = 0; i < pairs; ++i) {
    queue.push(std::move(frame));
    std::optional<FramePtr> popped = queue.pop(std::chrono::milliseconds(0));
    if (!popped) throw std::logic_error("the queue lost the frame");  // uncontended: never
    frame = std::move(*popped);
  }
  const std::chrono::duration<double, std::nano> elapsed = clock.now() - start;
  return elapsed.count() / static_cast<double>(pairs);
}

// Nanoseconds of metrics work per frame: record_frame + two depth samples. A fresh store per
// sample, so every id is recorded once (as in a run) and nothing reallocates.
double time_metrics(std::size_t frames, frameflow::Clock& clock) {
  frameflow::MetricsStore store(frames);
  frameflow::FrameMeta meta;
  const frameflow::Clock::time_point now = clock.now();
  meta.t_cap = meta.t_push = meta.t_pop = meta.t_proc_start = meta.t_proc_end = meta.t_out = now;
  const frameflow::Clock::time_point start = clock.now();
  for (std::size_t id = 0; id < frames; ++id) {
    meta.id = id;
    store.record_depth(now, 1);
    store.record_depth(now, 0);
    store.record_frame(meta);
  }
  const std::chrono::duration<double, std::nano> elapsed = clock.now() - start;
  return elapsed.count() / static_cast<double>(frames);
}

void print(const char* what, std::vector<double> ns, std::size_t pairs) {
  std::sort(ns.begin(), ns.end());
  std::printf("%s: median %.1f ns, min %.1f ns, max %.1f ns over %zu samples of %zu\n", what,
              ns[ns.size() / 2], ns.front(), ns.back(), ns.size(), pairs);
}

int run(int argc, char** argv) {
  const std::size_t samples = parse_count(argc > 1 ? argv[1] : nullptr, 101);
  const std::size_t pairs = parse_count(argc > 2 ? argv[2] : nullptr, 10000);

  frameflow::SteadyClock clock;
  frameflow::BlockingPolicy policy;
  frameflow::BoundedQueue<FramePtr> queue(8, policy);
  FramePtr frame = std::make_unique<frameflow::Frame>(0, cv::Mat(2, 2, CV_8UC1, cv::Scalar(0)));

  time_pairs(queue, frame, pairs, clock);  // warm-up: caches, branch predictors, first lock
  std::vector<double> ns(samples);
  for (double& sample : ns) sample = time_pairs(queue, frame, pairs, clock);
  print("push+pop pair (uncontended, blocking policy, capacity 8)", ns, pairs);

  time_metrics(pairs, clock);  // warm-up
  for (double& sample : ns) sample = time_metrics(pairs, clock);
  print("metrics per frame (record_frame + 2 x record_depth)", ns, pairs);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    (void)std::fprintf(stderr, "frameflow_bench_queue: error: %s\n", e.what());
    return 2;
  }
}
