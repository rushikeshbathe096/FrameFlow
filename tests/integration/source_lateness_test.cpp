// D38 / ER-07: the "source was late; system may be overloaded" warning must reflect the source's
// own lateness, not the backpressure a blocking queue applies by design.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "frameflow/metrics/run_report.hpp"
#include "frameflow/metrics/warnings.hpp"
#include "frameflow/pipeline.hpp"
#include "frameflow/sink/null_sink.hpp"
#include "frameflow/source/synthetic_source.hpp"
#include "invariants.hpp"

namespace frameflow {
namespace {

using std::chrono::milliseconds;

// A source that needs `work` per frame (like a slow decoder on a busy CPU).
class SlowSource final : public ISource {
 public:
  explicit SlowSource(milliseconds work) : work_(work) {}
  FramePtr next(std::uint64_t id) override {
    std::this_thread::sleep_for(work_);
    return inner_.next(id);
  }

 private:
  milliseconds work_;
  SyntheticSource inner_{cv::Size(64, 48)};
};

struct LatenessRun {
  RunReport report;
  std::vector<std::string> warnings;
};

LatenessRun run(const std::string& policy, double fps, milliseconds cost,
                std::unique_ptr<ISource> source) {
  SteadyClock clock;
  PipelineConfig config;
  config.fps = fps;
  config.duration = milliseconds(3000);
  config.cost = cost;
  config.policy = policy;
  Pipeline pipeline(config, clock, std::move(source), std::make_unique<NullSink>(clock));
  const RunResult result = pipeline.run();
  testing::expect_run_invariants(pipeline.metrics(), result.produced);
  RunConfig run_config;  // only the lateness rule is checked below
  run_config.fps = fps;
  run_config.duration_s = 3.0;
  run_config.warmup_s = 0.0;
  LatenessRun out;
  out.report = make_run_report(pipeline.metrics(), result, Clock::duration::zero());
  out.warnings = collect_warnings(run_config, out.report);
  return out;
}

double fraction_late(const RunReport& report, bool own_only) {
  std::size_t late = 0;
  for (const FrameMeta& meta : report.frames) {
    const Clock::duration lateness = own_only ? meta.own_lateness() : meta.source_lateness;
    if (lateness > milliseconds(5)) ++late;
  }
  return static_cast<double>(late) / static_cast<double>(report.frames.size());
}

bool warns_late(const LatenessRun& r) {
  for (const std::string& w : r.warnings) {
    if (w.rfind("source was late", 0) == 0) return true;
  }
  return false;
}

TEST(SourceLatenessTest, BlockingBackpressureAtOnePointFiveXDoesNotWarn) {
  // The reported case: 30 fps, 50 ms cost, blocking. The source waits on the full queue, so
  // most releases are late, but by backpressure only.
  const LatenessRun r =
      run("blocking", 30.0, milliseconds(50), std::make_unique<SyntheticSource>(cv::Size(64, 48)));
  EXPECT_GT(fraction_late(r.report, false), 0.5);  // the situation that used to warn
  EXPECT_LT(fraction_late(r.report, true), 0.01);
  EXPECT_FALSE(warns_late(r)) << r.warnings.front();
}

TEST(SourceLatenessTest, GenuinelySlowSourceWarns) {
  // 4 ms interval, 10 ms of source work per frame, no cost: the queue never fills, so all of
  // the lateness is the source's own (EC-12).
  const LatenessRun r =
      run("blocking", 250.0, milliseconds(0), std::make_unique<SlowSource>(milliseconds(10)));
  EXPECT_GT(fraction_late(r.report, true), 0.5);
  EXPECT_TRUE(warns_late(r));
}

TEST(SourceLatenessTest, SlowSourceUnderBlockingBackpressureStillWarns) {
  // 1.5x overload plus a source that needs 45 ms per frame at a 33 ms interval: backpressure
  // must not hide the source's own slowness.
  const LatenessRun r =
      run("blocking", 30.0, milliseconds(50), std::make_unique<SlowSource>(milliseconds(45)));
  EXPECT_TRUE(warns_late(r));
}

}  // namespace
}  // namespace frameflow
