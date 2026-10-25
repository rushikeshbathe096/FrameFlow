// config_file.hpp
// Responsibility: reads YAML configuration files (VR-18, SR-07) into the same raw option strings
// the flags produce, so one validate() applies the same rules to both (D40). The schema is the
// UIUX 8.3 template: `source: {type, resolution, fps, input, loop}`, `processor: {type, kernel}`,
// `duration_s`, `frames`, `warmup_s`, `seed`, `cost_ms`, `cost_mode`, `cost_wait`, `spike_ms`,
// `spike_len`, `spike_every`, `queue`, `policy`, `adaptive: {target_ms, exit_ratio, window}`.
// Unknown keys are rejected with `file:line:col` and the closest valid key; files over 1 MB and
// aliases that expand beyond 1 MB are rejected. Flags override file values (FR-CLI-01, EC-19).
// Bench files add `experiment`, `repeats` and `matrix` (bench/experiment.hpp), which `run --config`
// refuses. Thread-safety: pure functions; called on the main thread before any pipeline thread
// starts.
#pragma once

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "frameflow/config.hpp"

namespace frameflow {

inline constexpr std::uintmax_t kMaxConfigBytes = std::uintmax_t{1024} * 1024;  // SR-07

// Every problem as "<file>:<line>:<col>: <problem>" (or "<file>: <problem>"), 1-based positions.
using ConfigErrors = std::vector<std::string>;

// Reads and parses a YAML file: must exist, be a regular file of at most 1 MB, parse, and expand
// (aliases included) to at most 1 MB of scalar text. std::nullopt (with errors) otherwise.
std::optional<YAML::Node> load_yaml_file(const std::filesystem::path& path, ConfigErrors& errors);

// "<file>:<line>:<col>: " for a node.
std::string position(const std::string& file, const YAML::Node& node);

// The dotted key paths a run setting can have, in schema order (e.g. "source.fps", "cost_ms").
const std::vector<std::string>& run_key_paths();

// Sets the RawConfig field for a dotted key path. False if the path is unknown.
bool set_run_value(RawConfig& raw, const std::string& path, const std::string& value);

// Sets the RawConfig field for a dotted key path from a scalar node. False if path is unknown.
bool set_run_key(RawConfig& raw, const std::string& path, const YAML::Node& value,
                 const std::string& file, ConfigErrors& errors);

// Reads every run key of a mapping root into raw. Root keys listed in `extra_keys` are skipped
// (left to the caller); any other unknown key is an error.
void read_run_keys(const YAML::Node& root, const std::string& file,
                   const std::vector<std::string>& extra_keys, RawConfig& raw,
                   ConfigErrors& errors);

struct ConfigFileResult {
  RawConfig raw;
  ConfigErrors errors;
  bool ok() const { return errors.empty(); }
};

// `frameflow run --config=<file>`.
ConfigFileResult load_run_config(const std::filesystem::path& path);

// Every field set in overrides replaces the one in base (flags beat the file, EC-19).
RawConfig merge_raw(const RawConfig& base, const RawConfig& overrides);

}  // namespace frameflow
