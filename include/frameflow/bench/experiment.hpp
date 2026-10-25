// experiment.hpp
// Responsibility: bench experiment files (UIUX 8.3, DR-05 matrix part). An experiment file is a
// run config (config_file.hpp schema) plus `experiment` (folder name), `repeats` (VR-19, default
// 5) and `matrix` (key -> list of values). The matrix expands to the cartesian product in file
// order, the last key varying fastest, and every configuration gets a stable folder name such as
// `cost_ms-40_policy-blocking` (D41). Each expanded configuration is validated like a run.
// Also builds `experiment.json`: the matrix, the configurations and every run's status (EC-20).
// Thread-safety: pure functions; called on the main thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "frameflow/config.hpp"
#include "frameflow/config_file.hpp"

namespace frameflow {

inline constexpr std::size_t kMaxConfigurations = 1000;

using Params = std::vector<std::pair<std::string, std::string>>;  // key -> value, file order
using Matrix = std::vector<std::pair<std::string, std::vector<std::string>>>;  // file order

struct ExperimentConfig {
  std::string name;  // folder name, e.g. "policy-blocking"
  Params params;
  RunConfig config;  // validated
};

struct Experiment {
  std::string name;
  std::uint64_t repeats = 5;
  Matrix matrix;
  std::vector<ExperimentConfig> configs;  // expansion order
};

struct ExperimentLoad {
  Experiment experiment;
  ConfigErrors errors;
  bool ok() const { return errors.empty(); }
};

// repeats_override: `frameflow bench --repeats` (validated by VR-19 here too).
ExperimentLoad load_experiment(const std::filesystem::path& path,
                               const std::optional<std::string>& repeats_override = std::nullopt);

struct MatrixPoint {
  Params params;
  RawConfig raw;  // base with this point's matrix values applied
};

// Cartesian product of the matrix over base, the last key varying fastest. Matrix keys must be
// run key paths (run_key_paths()).
std::vector<MatrixPoint> expand_matrix(const RawConfig& base, const Matrix& matrix);

// "cost_ms-40_policy-blocking": the last part of each key, '-', the value with characters other
// than letters, digits, '.' and '-' replaced by '-'; "default" for an empty matrix.
std::string config_name(const Params& params);

// Interrupted: stopped by a signal part-way (its folder holds partial, invalid results); it is
// listed under "missing" with the runs that never started, since both must be run again (EC-20).
enum class RunStatus { Completed, Failed, Interrupted, Missing };

struct RunRecord {
  std::string config;     // ExperimentConfig::name
  std::uint64_t rep = 1;  // 1-based, folder rep_<rep>
  RunStatus status = RunStatus::Missing;
  int exit_code = 0;  // meaningful for Completed/Failed
};

nlohmann::json make_experiment_json(const Experiment& experiment,
                                    const std::vector<RunRecord>& runs, bool interrupted);

}  // namespace frameflow
