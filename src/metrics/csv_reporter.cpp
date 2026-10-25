#include "frameflow/metrics/csv_reporter.hpp"

#include <charconv>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "frameflow/platform/output_dir.hpp"

namespace frameflow {

namespace {

// Locale-independent number formatting (DR-07).
void put_int(std::ostream& out, long long value) {
  char buffer[24];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.write(buffer, result.ptr - buffer);
}

void put_fixed3(std::ostream& out, double value) {
  char buffer[48];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 3);
  out.write(buffer, result.ptr - buffer);
}

void put_time_us(std::ostream& out, const std::optional<Clock::time_point>& t,
                 Clock::time_point t0) {
  if (t) put_int(out, std::chrono::duration_cast<std::chrono::microseconds>(*t - t0).count());
}

void put_frame_row(std::ostream& out, const FrameMeta& meta, Clock::time_point t0) {
  put_int(out, static_cast<long long>(meta.id));
  for (const auto* t : {&meta.t_cap, &meta.t_push, &meta.t_pop, &meta.t_proc_start,
                        &meta.t_proc_end, &meta.t_out}) {
    out << ',';
    put_time_us(out, *t, t0);
  }
  out << ',';
  if (meta.t_out && meta.t_cap) {
    put_fixed3(out, std::chrono::duration<double, std::milli>(*meta.t_out - *meta.t_cap).count());
  }
  out << ',';
  put_int(out, std::chrono::duration_cast<std::chrono::microseconds>(meta.source_lateness).count());
  out << ',' << (meta.is_dropped() ? '1' : '0') << ',' << drop_reason_name(meta.drop_reason) << ','
      << (meta.warmup ? '1' : '0') << '\n';
}

}  // namespace

void write_frames_csv(std::ostream& out, const std::vector<FrameMeta>& frames,
                      Clock::time_point t0) {
  out << "id,t_cap_us,t_push_us,t_pop_us,t_proc_start_us,t_proc_end_us,t_out_us,latency_ms,"
         "lateness_us,dropped,reason,warmup\n";
  for (const FrameMeta& meta : frames) put_frame_row(out, meta, t0);
}

void write_depth_csv(std::ostream& out, const std::vector<DepthSample>& samples,
                     Clock::time_point t0) {
  out << "t_us,depth\n";
  for (const DepthSample& sample : samples) {
    put_time_us(out, sample.t, t0);
    out << ',';
    put_int(out, static_cast<long long>(sample.depth));
    out << '\n';
  }
}

void write_events_csv(std::ostream& out, const std::vector<RunEvent>& events,
                      Clock::time_point t0) {
  out << "t_us,type,frame_id,detail\n";
  for (const RunEvent& event : events) {
    put_time_us(out, event.t, t0);
    out << ',' << event.type << ',';
    if (event.frame_id) put_int(out, static_cast<long long>(*event.frame_id));
    out << ',' << event.detail << '\n';
  }
}

void write_csv_files(const std::filesystem::path& dir, const RunReport& report) {
  write_result_file(dir / "frames.csv",
                    [&](std::ostream& out) { write_frames_csv(out, report.frames, report.t0); });
  write_result_file(dir / "depth.csv",
                    [&](std::ostream& out) { write_depth_csv(out, report.depth, report.t0); });
  write_result_file(dir / "events.csv",
                    [&](std::ostream& out) { write_events_csv(out, report.events, report.t0); });
}

}  // namespace frameflow
