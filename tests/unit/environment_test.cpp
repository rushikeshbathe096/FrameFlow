#include "frameflow/platform/environment.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <climits>
#include <string>

namespace frameflow {
namespace {

TEST(EnvironmentTest, ParsesCpuModelFromFirstProcessor) {
  const std::string cpuinfo =
      "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel(R) Core(TM) i7 CPU\n"
      "\nprocessor\t: 1\nmodel name\t: Intel(R) Core(TM) i7 CPU\n";
  EXPECT_EQ(parse_cpu_model(cpuinfo), "Intel(R) Core(TM) i7 CPU");
}

TEST(EnvironmentTest, CpuModelUnknownWhenMissing) {
  // e.g. some ARM kernels have no "model name" line
  EXPECT_EQ(parse_cpu_model("processor\t: 0\nBogoMIPS\t: 48.00\n"), "unknown");
  EXPECT_EQ(parse_cpu_model(""), "unknown");
  EXPECT_EQ(parse_cpu_model("model name\t:   \n"), "unknown");
}

TEST(EnvironmentTest, ParsesMemTotalInMegabytes) {
  const std::string meminfo = "MemTotal:       16314832 kB\nMemFree:         1234567 kB\n";
  EXPECT_EQ(parse_mem_total_mb(meminfo), 16314832u / 1024u);
}

TEST(EnvironmentTest, MemTotalEmptyWhenMissingOrMalformed) {
  EXPECT_EQ(parse_mem_total_mb(""), std::nullopt);
  EXPECT_EQ(parse_mem_total_mb("MemFree: 100 kB\n"), std::nullopt);
  EXPECT_EQ(parse_mem_total_mb("MemTotal: lots kB\n"), std::nullopt);
  EXPECT_EQ(parse_mem_total_mb("MemTotal: 100 MB\n"), std::nullopt);
}

TEST(EnvironmentTest, ParsesKbFieldsOfProcStatus) {  // D56
  const std::string status = "Name:\tframeflow\nVmHWM:\t   69720 kB\nVmRSS:\t   6972 kB\n";
  EXPECT_EQ(parse_kb_field(status, "VmRSS"), 6972u);
  EXPECT_EQ(parse_kb_field(status, "VmHWM"), 69720u);
  EXPECT_EQ(parse_kb_field(status, "VmSwap"), std::nullopt);
  EXPECT_EQ(parse_kb_field("VmRSS: lots kB\n", "VmRSS"), std::nullopt);
}

TEST(EnvironmentTest, ReadsOwnMemoryStatus) {
  const MemoryStatus status = read_memory_status();
  ASSERT_TRUE(status.rss_kb.has_value());
  ASSERT_TRUE(status.peak_kb.has_value());
  EXPECT_GT(*status.rss_kb, 0u);
  EXPECT_GE(*status.peak_kb, *status.rss_kb);  // the peak is never below the current value
}

TEST(EnvironmentTest, HostnameHashMatchesFnv1aReferenceValues) {
  // Published FNV-1a 64-bit test vectors.
  EXPECT_EQ(hash_hostname(""), "cbf29ce484222325");
  EXPECT_EQ(hash_hostname("a"), "af63dc4c8601ec8c");
  EXPECT_EQ(hash_hostname("foobar"), "85944171f73967e8");
}

TEST(EnvironmentTest, HostnameHashIsStableAndDistinguishesNames) {
  EXPECT_EQ(hash_hostname("lab-pc-01"), hash_hostname("lab-pc-01"));
  EXPECT_NE(hash_hostname("lab-pc-01"), hash_hostname("lab-pc-02"));
  EXPECT_EQ(hash_hostname("lab-pc-01").size(), 16u);
}

TEST(EnvironmentTest, CollectsRealMachineWithoutPlainHostname) {
  const Environment env = collect_environment();
  EXPECT_NE(env.cpu_model, "");
  EXPECT_GT(env.logical_cores, 0u);
  ASSERT_TRUE(env.total_ram_mb.has_value());  // Linux always has /proc/meminfo
  EXPECT_GT(*env.total_ram_mb, 0u);
  EXPECT_NE(env.kernel, "unknown");
  EXPECT_NE(env.compiler, "unknown");
  EXPECT_NE(env.opencv_version, "unknown");

  char host[HOST_NAME_MAX + 1] = {};
  ASSERT_EQ(gethostname(host, sizeof(host) - 1), 0);
  EXPECT_EQ(env.hostname_hash, hash_hostname(host));
  EXPECT_NE(env.hostname_hash, std::string(host));
}

}  // namespace
}  // namespace frameflow
