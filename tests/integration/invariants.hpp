// invariants.hpp (test helper)
// Responsibility: checks the run invariants every integration test must satisfy:
// BR-01 frame conservation, BR-02 one category per frame, BR-03 delivery order, BR-04 timestamp
// order. Call after Pipeline::run() has returned.
// Thread-safety: not thread-safe; reads a finished MetricsStore.
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "frameflow/frame.hpp"
#include "frameflow/metrics/metrics_store.hpp"

namespace frameflow::testing {

struct FrameCounts {
  std::uint64_t delivered = 0;
  std::uint64_t dropped = 0;
  std::uint64_t in_flight = 0;
};

inline bool is_delivered(const FrameMeta& meta) { return meta.t_out.has_value(); }

inline FrameCounts count_frames(const std::vector<FrameMeta>& frames) {
  FrameCounts counts;
  for (const FrameMeta& meta : frames) {
    if (meta.is_dropped()) {
      ++counts.dropped;
    } else if (is_delivered(meta)) {
      ++counts.delivered;
    } else {
      ++counts.in_flight;
    }
  }
  return counts;
}

inline void expect_ordered(const std::optional<Clock::time_point>& earlier,
                           const std::optional<Clock::time_point>& later, std::uint64_t id) {
  ASSERT_TRUE(earlier.has_value() && later.has_value()) << "frame " << id;
  EXPECT_LE(*earlier, *later) << "frame " << id;
}

// Returns the counts so callers can add policy-specific expectations.
inline FrameCounts expect_run_invariants(const MetricsStore& metrics, std::uint64_t produced) {
  const std::vector<FrameMeta> frames = metrics.frames();

  // BR-01: every produced frame has exactly one record, ids 0..produced-1.
  EXPECT_EQ(frames.size(), produced);
  for (std::uint64_t i = 0; i < frames.size(); ++i) EXPECT_EQ(frames[i].id, i);

  const FrameCounts counts = count_frames(frames);
  EXPECT_EQ(counts.delivered + counts.dropped + counts.in_flight, produced);

  const FrameMeta* previous_delivered = nullptr;
  for (const FrameMeta& meta : frames) {
    // BR-02: a dropped frame never reaches the sink.
    if (meta.is_dropped()) {
      EXPECT_FALSE(is_delivered(meta)) << "frame " << meta.id;
    }
    EXPECT_TRUE(meta.t_cap.has_value()) << "frame " << meta.id;
    if (!is_delivered(meta) || meta.is_dropped()) continue;

    // BR-04: t_cap <= t_push <= t_pop <= t_proc_start <= t_proc_end <= t_out.
    expect_ordered(meta.t_cap, meta.t_push, meta.id);
    expect_ordered(meta.t_push, meta.t_pop, meta.id);
    expect_ordered(meta.t_pop, meta.t_proc_start, meta.id);
    expect_ordered(meta.t_proc_start, meta.t_proc_end, meta.id);
    expect_ordered(meta.t_proc_end, meta.t_out, meta.id);

    // BR-03: delivered in strictly increasing id order (ids ascend; output times must too).
    if (previous_delivered != nullptr) {
      EXPECT_LE(*previous_delivered->t_out, *meta.t_out);
    }
    previous_delivered = &meta;
  }
  return counts;
}

}  // namespace frameflow::testing
