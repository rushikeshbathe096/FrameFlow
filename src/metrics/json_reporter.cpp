#include "frameflow/metrics/json_reporter.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>

#include "frameflow/platform/output_dir.hpp"
#include "frameflow/version.hpp"

namespace frameflow {

namespace {

using nlohmann::json;

template <typename T>
json or_null(const std::optional<T>& value) {
  return value ? json(*value) : json(nullptr);
}

const char* ended_by_name(EndedBy ended_by) {
  switch (ended_by) {
    case EndedBy::Duration:
      return "duration";
    case EndedBy::Frames:
      return "frames";
    case EndedBy::InputEnd:
      return "input_eof";
    case EndedBy::StopRequested:
      return "stop_requested";
    case EndedBy::Error:
      break;
  }
  return "error";
}

// First spike's offset from the run start (D39), so analysis can rebuild the spike schedule (D46).
std::optional<double> spike_offset_s(const RunConfig& config) {
  if (!config.spike_every_s) return std::nullopt;
  const PipelineConfig pipeline = config.to_pipeline_config();
  if (!pipeline.burst) return std::nullopt;
  return std::chrono::duration<double>(first_spike_offset(pipeline.burst->every, config.seed))
      .count();
}

// SR-08: only the file name, never a path outside the output folder.
json input_name(const std::optional<std::string>& input) {
  if (!input) return nullptr;
  return std::filesystem::path(*input).filename().string();
}

json config_json(const RunConfig& config) {
  return {{"fps", config.fps},
          {"queue", config.queue},
          {"duration_s", or_null(config.duration_s)},
          {"frames", or_null(config.frames)},
          {"warmup_s", config.warmup_s},
          {"processor", config.processor},
          {"kernel", config.processor == "blur" ? json(config.kernel) : json(nullptr)},
          {"cost_ms", config.cost_ms},
          {"cost_mode", config.cost_mode},
          {"cost_wait", config.cost_wait == CostWait::Busy ? "busy" : "sleep"},
          {"spike_ms", or_null(config.spike_ms)},
          {"spike_len_s", or_null(config.spike_len_s)},
          {"spike_every_s", or_null(config.spike_every_s)},
          {"spike_offset_s", or_null(spike_offset_s(config))},
          {"policy", config.policy},
          {"target_ms", or_null(config.target_ms)},
          {"exit_ratio", or_null(config.exit_ratio)},
          {"window", or_null(config.window)},
          {"resolution", std::to_string(config.resolution.width) + "x" +
                             std::to_string(config.resolution.height)},
          {"source", config.source},
          {"input", input_name(config.input)},
          {"loop", config.loop},
          {"seed", config.seed}};
}

json environment_json(const Environment& env) {
  return {{"cpu_model", env.cpu_model},
          {"logical_cores", env.logical_cores},
          {"total_ram_mb", or_null(env.total_ram_mb)},
          {"kernel", env.kernel},
          {"compiler", env.compiler},
          {"opencv_version", env.opencv_version},
          {"hostname_hash", env.hostname_hash}};
}

json metrics_json(const RunStats& s) {
  return {{"produced", s.produced},
          {"delivered", s.delivered},
          {"dropped", s.dropped},
          {"in_flight", s.in_flight},
          {"window_s", s.window_s},
          {"window_produced", s.window_produced},
          {"window_delivered", s.window_delivered},
          {"window_dropped_oldest", s.window_dropped_oldest},
          {"window_dropped_newest", s.window_dropped_newest},
          {"window_dropped_stale", s.window_dropped_stale},
          {"p50_ms", or_null(s.p50_ms)},
          {"p95_ms", or_null(s.p95_ms)},
          {"p99_ms", or_null(s.p99_ms)},
          {"max_ms", or_null(s.max_ms)},
          {"input_fps", or_null(s.input_fps)},
          {"output_fps", or_null(s.output_fps)},
          {"drop_rate_pct", or_null(s.drop_rate_pct)},
          {"drop_oldest_pct", or_null(s.drop_oldest_pct)},
          {"drop_newest_pct", or_null(s.drop_newest_pct)},
          {"drop_stale_pct", or_null(s.drop_stale_pct)},
          {"depth_mean", or_null(s.depth_mean)},
          {"depth_max", or_null(s.depth_max)},
          {"state_changes", or_null(s.state_changes)},
          {"shedding_pct", or_null(s.shedding_pct)}};
}

json memory_json(const MemoryUsage& m) {
  return {{"peak_rss_mb", or_null(m.peak_rss_mb)},
          {"rss_after_warmup_mb", or_null(m.rss_after_warmup_mb)},
          {"rss_end_mb", or_null(m.rss_end_mb)},
          {"growth_pct", or_null(m.growth_pct)}};
}

}  // namespace

nlohmann::json make_summary_json(const RunConfig& config, const RunReport& report,
                                 const Environment& env, const std::vector<std::string>& warnings) {
  return {{"schema_version", kSummarySchemaVersion},
          {"frameflow_version", version()},
          {"git_commit", git_commit()},
          {"valid", report.valid},
          {"ended_by", ended_by_name(report.ended_by)},
          {"error", report.error.empty() ? json(nullptr) : json(report.error)},
          {"warnings", warnings},
          {"config", config_json(config)},
          {"environment", environment_json(env)},
          {"metrics", metrics_json(report.stats)},
          {"memory", memory_json(report.memory)}};
}

void write_summary_json(const std::filesystem::path& dir, const nlohmann::json& summary) {
  write_result_file(dir / "summary.json", [&](std::ostream& out) {
    // replace: invalid UTF-8 (e.g. in an error message) must not make the dump throw.
    out << summary.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
  });
}

}  // namespace frameflow
