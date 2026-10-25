#include "frameflow/bench/experiment.hpp"

#include <algorithm>
#include <cctype>

#include "frameflow/cli.hpp"
#include "frameflow/version.hpp"

namespace frameflow {

namespace {

const std::vector<std::string>& experiment_keys() {
  static const std::vector<std::string> keys = {"experiment", "repeats", "matrix"};
  return keys;
}

bool safe_folder_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-' || c == '.';
  });
}

void read_name(const YAML::Node& root, const std::string& file, Experiment& e,
               ConfigErrors& errors) {
  const YAML::Node node = root["experiment"];
  if (!node) {
    errors.push_back(file + ": missing 'experiment' (the folder name under --out)");
  } else if (!node.IsScalar() || !safe_folder_name(node.Scalar())) {
    errors.push_back(position(file, node) +
                     "'experiment' must be a folder name of letters, digits, '_', '-' and '.'");
  } else {
    e.name = node.Scalar();
  }
}

// VR-19: 1 to 100, default 5; `--repeats` beats the file.
void read_repeats(const YAML::Node& root, const std::string& file,
                  const std::optional<std::string>& override_text, Experiment& e,
                  ConfigErrors& errors) {
  const YAML::Node node = root["repeats"];
  std::optional<std::string> text = override_text;
  std::string where = "--repeats ";
  if (!text && node) {
    if (!node.IsScalar()) {
      errors.push_back(position(file, node) + "'repeats' must be a single value");
      return;
    }
    text = node.Scalar();
    where = position(file, node) + "repeats ";
  }
  if (!text) return;
  const std::optional<std::uint64_t> repeats = parse_unsigned(*text);
  if (!repeats || *repeats < 1 || *repeats > 100) {
    errors.push_back(where + *text + " must be a whole number between 1 and 100 (hint: try 5)");
    return;
  }
  e.repeats = *repeats;
}

bool read_values(const YAML::Node& values, const std::string& key, const std::string& file,
                 std::vector<std::string>& out, ConfigErrors& errors) {
  if (!values.IsSequence() || values.size() == 0) {
    errors.push_back(position(file, values) + "matrix '" + key +
                     "' must be a non-empty list, like [40, 50, 67]");
    return false;
  }
  for (const YAML::Node& value : values) {
    if (!value.IsScalar()) {
      errors.push_back(position(file, value) + "matrix '" + key + "' values must be single values");
      return false;
    }
    if (std::find(out.begin(), out.end(), value.Scalar()) != out.end()) {
      errors.push_back(position(file, value) + "matrix '" + key + "' lists '" + value.Scalar() +
                       "' twice");
      return false;
    }
    out.push_back(value.Scalar());
  }
  return true;
}

void read_matrix(const YAML::Node& root, const std::string& file, Experiment& e,
                 ConfigErrors& errors) {
  const YAML::Node matrix = root["matrix"];
  if (!matrix) return;  // a single configuration
  if (!matrix.IsMap()) {
    errors.push_back(position(file, matrix) + "'matrix' must map keys to lists of values");
    return;
  }
  const std::vector<std::string>& paths = run_key_paths();
  for (const auto& entry : matrix) {
    const std::string key = entry.first.Scalar();
    if (std::find(paths.begin(), paths.end(), key) == paths.end()) {
      errors.push_back(position(file, entry.first) + "unknown matrix key '" + key +
                       "' (did you mean '" + closest_flag(key, paths) + "'?)");
      continue;
    }
    std::vector<std::string> values;
    if (read_values(entry.second, key, file, values, errors)) e.matrix.emplace_back(key, values);
  }
}

std::string describe(const ValidationError& error) {
  std::string text =
      error.option + (error.value.empty() ? "" : " " + error.value) + " " + error.problem;
  return text + " (hint: " + error.hint + ")";
}

// D45: the file-level `adaptive:` section applies only to configurations whose policy is
// adaptive; adaptive keys in the matrix are kept, so pairing them with a fixed policy is an error.
void keep_adaptive_settings_for_adaptive(MatrixPoint& point) {
  if (point.raw.policy && *point.raw.policy == "adaptive") return;
  using Field = std::optional<std::string> RawConfig::*;
  const std::pair<const char*, Field> keys[] = {{"adaptive.target_ms", &RawConfig::target_ms},
                                                {"adaptive.exit_ratio", &RawConfig::exit_ratio},
                                                {"adaptive.window", &RawConfig::window}};
  for (const auto& entry : keys) {
    const std::string_view key = entry.first;  // a plain variable: lambdas may capture it (C++17)
    const bool from_matrix = std::any_of(point.params.begin(), point.params.end(),
                                         [key](const auto& param) { return param.first == key; });
    if (!from_matrix) (point.raw.*entry.second).reset();
  }
}

void build_configs(const RawConfig& base, const std::string& file, Experiment& e,
                   ConfigErrors& errors) {
  std::size_t count = 1;
  for (const auto& [key, values] : e.matrix) count *= values.size();
  if (count > kMaxConfigurations) {
    errors.push_back(file + ": the matrix has " + std::to_string(count) +
                     " configurations; at most 1000 are allowed");
    return;
  }
  for (MatrixPoint& point : expand_matrix(base, e.matrix)) {
    keep_adaptive_settings_for_adaptive(point);
    const std::string name = config_name(point.params);
    const ValidationResult validated = validate(point.raw);
    for (const ValidationError& error : validated.errors) {
      errors.push_back(file + ": configuration " + name + ": " + describe(error));
    }
    const bool duplicate = std::any_of(e.configs.begin(), e.configs.end(),
                                       [&](const ExperimentConfig& c) { return c.name == name; });
    if (duplicate) errors.push_back(file + ": two configurations are both named " + name);
    e.configs.push_back({name, std::move(point.params), validated.config});
  }
}

const char* status_name(RunStatus status) {
  switch (status) {
    case RunStatus::Completed:
      return "completed";
    case RunStatus::Failed:
      return "failed";
    case RunStatus::Interrupted:
      return "interrupted";
    case RunStatus::Missing:
      break;
  }
  return "missing";
}

}  // namespace

ExperimentLoad load_experiment(const std::filesystem::path& path,
                               const std::optional<std::string>& repeats_override) {
  ExperimentLoad load;
  const std::string file = path.string();
  const std::optional<YAML::Node> root = load_yaml_file(path, load.errors);
  if (!root) return load;
  RawConfig base;
  read_run_keys(*root, file, experiment_keys(), base, load.errors);
  if (!root->IsMap()) return load;
  read_name(*root, file, load.experiment, load.errors);
  read_repeats(*root, file, repeats_override, load.experiment, load.errors);
  read_matrix(*root, file, load.experiment, load.errors);
  if (load.ok()) build_configs(base, file, load.experiment, load.errors);
  return load;
}

std::vector<MatrixPoint> expand_matrix(const RawConfig& base, const Matrix& matrix) {
  std::vector<MatrixPoint> points = {MatrixPoint{{}, base}};
  for (const auto& [key, values] : matrix) {  // each key multiplies the points so far
    std::vector<MatrixPoint> next;
    for (const MatrixPoint& point : points) {
      for (const std::string& value : values) {
        MatrixPoint extended = point;
        extended.params.emplace_back(key, value);
        set_run_value(extended.raw, key, value);
        next.push_back(std::move(extended));
      }
    }
    points = std::move(next);
  }
  return points;
}

std::string config_name(const Params& params) {
  if (params.empty()) return "default";
  std::string name;
  for (const auto& [key, value] : params) {
    std::string safe = value;
    for (char& c : safe) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') c = '-';
    }
    if (!name.empty()) name += '_';
    name += key.substr(key.rfind('.') + 1) + "-" + safe;
  }
  return name;
}

nlohmann::json make_experiment_json(const Experiment& experiment,
                                    const std::vector<RunRecord>& runs, bool interrupted) {
  nlohmann::json matrix = nlohmann::json::array();
  for (const auto& [key, values] : experiment.matrix) {
    matrix.push_back({{"key", key}, {"values", values}});
  }
  nlohmann::json configs = nlohmann::json::array();
  for (const ExperimentConfig& c : experiment.configs) {
    nlohmann::json params = nlohmann::json::object();
    for (const auto& [key, value] : c.params) params[key] = value;
    configs.push_back({{"name", c.name}, {"params", params}});
  }
  nlohmann::json run_list = nlohmann::json::array();
  nlohmann::json missing = nlohmann::json::array();
  nlohmann::json failed = nlohmann::json::array();
  std::size_t completed = 0;
  for (const RunRecord& run : runs) {
    const std::string path = run.config + "/rep_" + std::to_string(run.rep);
    nlohmann::json entry = {{"config", run.config},
                            {"rep", run.rep},
                            {"path", path},
                            {"status", status_name(run.status)}};
    if (run.status != RunStatus::Missing) entry["exit_code"] = run.exit_code;
    run_list.push_back(entry);
    if (run.status == RunStatus::Completed) ++completed;
    if (run.status == RunStatus::Missing || run.status == RunStatus::Interrupted) {
      missing.push_back(path);
    }
    if (run.status == RunStatus::Failed) failed.push_back(path);
  }
  return {{"schema_version", 1},
          {"experiment", experiment.name},
          {"frameflow_version", version()},
          {"git_commit", git_commit()},
          {"repeats", experiment.repeats},
          {"matrix", matrix},
          {"configurations", configs},
          {"runs", run_list},
          {"completed", completed},
          {"failed", failed},
          {"missing", missing},
          {"interrupted", interrupted}};
}

}  // namespace frameflow
