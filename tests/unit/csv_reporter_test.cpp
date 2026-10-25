#include "frameflow/metrics/csv_reporter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace frameflow {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(100);

FrameMeta delivered_frame() {
  FrameMeta meta;
  meta.id = 0;
  meta.t_cap = kT0;
  meta.t_push = kT0 + microseconds(12);
  meta.t_pop = kT0 + microseconds(1500);
  meta.t_proc_start = kT0 + microseconds(1510);
  meta.t_proc_end = kT0 + microseconds(51510);
  meta.t_out = kT0 + microseconds(51520);
  meta.source_lateness = microseconds(12);
  return meta;
}

FrameMeta in_flight_frame() {
  FrameMeta meta;
  meta.id = 1;
  meta.t_cap = kT0 + microseconds(33333);
  meta.t_push = kT0 + microseconds(1234567);  // late: exercises a 7-digit value
  return meta;
}

FrameMeta dropped_warmup_frame() {
  FrameMeta meta;
  meta.id = 2;
  meta.t_cap = kT0 + microseconds(66667);
  meta.t_push = kT0 + microseconds(66670);
  meta.source_lateness = microseconds(3);
  meta.drop_reason = DropReason::Oldest;
  meta.warmup = true;
  return meta;
}

const char* const kFramesGolden =
    "id,t_cap_us,t_push_us,t_pop_us,t_proc_start_us,t_proc_end_us,t_out_us,latency_ms,"
    "lateness_us,dropped,reason,warmup\n"
    "0,0,12,1500,1510,51510,51520,51.520,12,0,,0\n"
    "1,33333,1234567,,,,,,0,0,,0\n"
    "2,66667,66670,,,,,,3,1,oldest,1\n";

std::string frames_csv() {
  std::ostringstream out;
  write_frames_csv(out, {delivered_frame(), in_flight_frame(), dropped_warmup_frame()}, kT0);
  return out.str();
}

TEST(CsvReporterTest, FramesCsvMatchesGolden) { EXPECT_EQ(frames_csv(), kFramesGolden); }

TEST(CsvReporterTest, DepthCsvMatchesGolden) {
  std::ostringstream out;
  write_depth_csv(out, {{kT0 + microseconds(12), 1}, {kT0 + milliseconds(2), 0}}, kT0);
  EXPECT_EQ(out.str(), "t_us,depth\n12,1\n2000,0\n");
}

TEST(CsvReporterTest, EventsCsvMatchesGolden) {
  std::ostringstream out;
  write_events_csv(out,
                   {{kT0 + microseconds(5000), "drop", 3, "oldest"},
                    {kT0 + microseconds(7000), "drop", 4, "newest"}},
                   kT0);
  EXPECT_EQ(out.str(), "t_us,type,frame_id,detail\n5000,drop,3,oldest\n7000,drop,4,newest\n");
}

TEST(CsvReporterTest, EmptyInputsWriteHeaderOnly) {
  std::ostringstream frames;
  write_frames_csv(frames, {}, kT0);
  EXPECT_EQ(frames.str().find('\n'), frames.str().size() - 1);  // exactly one line
  std::ostringstream depth;
  write_depth_csv(depth, {}, kT0);
  EXPECT_EQ(depth.str(), "t_us,depth\n");
}

// DR-07: a locale with ',' as decimal point and '.' grouping must not change the output.
// The facet is built in code, so the test does not depend on installed system locales.
struct CommaDecimal : std::numpunct<char> {
  char do_decimal_point() const override { return ','; }
  char do_thousands_sep() const override { return '.'; }
  std::string do_grouping() const override { return "\3"; }
};

TEST(CsvReporterTest, OutputIgnoresLocale) {
  const std::locale comma(std::locale::classic(), new CommaDecimal);  // facet owned by locale
  const std::locale previous = std::locale::global(comma);
  std::ostringstream out;  // created after the change: picks up the global locale
  out.imbue(comma);
  write_frames_csv(out, {delivered_frame(), in_flight_frame(), dropped_warmup_frame()}, kT0);
  std::locale::global(previous);
  EXPECT_EQ(out.str(), kFramesGolden);
}

class CsvFilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() /
        ("frameflow_csv_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
  }

  std::filesystem::path dir_;
};

TEST_F(CsvFilesTest, WritesAllThreeFiles) {
  RunReport report;
  report.t0 = kT0;
  report.frames = {delivered_frame()};
  report.depth = {{kT0 + microseconds(12), 1}};
  write_csv_files(dir_, report);
  EXPECT_EQ(read(dir_ / "frames.csv"),
            "id,t_cap_us,t_push_us,t_pop_us,t_proc_start_us,t_proc_end_us,t_out_us,latency_ms,"
            "lateness_us,dropped,reason,warmup\n0,0,12,1500,1510,51510,51520,51.520,12,0,,0\n");
  EXPECT_EQ(read(dir_ / "depth.csv"), "t_us,depth\n12,1\n");
  EXPECT_EQ(read(dir_ / "events.csv"), "t_us,type,frame_id,detail\n");
}

TEST_F(CsvFilesTest, MissingDirectoryNamesTheFile) {
  try {
    write_csv_files(dir_ / "missing", RunReport{});
    FAIL() << "expected std::runtime_error";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("frames.csv"), std::string::npos) << e.what();
  }
}

}  // namespace
}  // namespace frameflow
