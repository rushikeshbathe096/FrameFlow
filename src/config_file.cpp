#include "frameflow/config_file.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "frameflow/cli.hpp"

namespace frameflow {

namespace {

namespace fs = std::filesystem;
using Field = std::optional<std::string> RawConfig::*;

// Schema order: dotted path -> RawConfig field (D40).
const std::vector<std::pair<std::string, Field>>& run_keys() {
  static const std::vector<std::pair<std::string, Field>> keys = {
      {"source.type", &RawConfig::source},
      {"source.resolution", &RawConfig::resolution},
      {"source.fps", &RawConfig::fps},
      {"source.input", &RawConfig::input},
      {"source.loop", &RawConfig::loop},
      {"processor.type", &RawConfig::processor},
      {"processor.kernel", &RawConfig::kernel},
      {"duration_s", &RawConfig::duration},
      {"frames", &RawConfig::frames},
      {"warmup_s", &RawConfig::warmup},
      {"seed", &RawConfig::seed},
      {"cost_ms", &RawConfig::cost_ms},
      {"cost_mode", &RawConfig::cost_mode},
      {"cost_wait", &RawConfig::cost_wait},
      {"spike_ms", &RawConfig::spike_ms},
      {"spike_len", &RawConfig::spike_len},
      {"spike_every", &RawConfig::spike_every},
      {"queue", &RawConfig::queue},
      {"policy", &RawConfig::policy},
      {"adaptive.target_ms", &RawConfig::target_ms},
      {"adaptive.exit_ratio", &RawConfig::exit_ratio},
      {"adaptive.window", &RawConfig::window},
  };
  return keys;
}

// The nested sections; their keys are the dotted paths above.
constexpr std::array<std::string_view, 3> kSections = {"source", "processor", "adaptive"};
constexpr std::array<std::string_view, 3> kExperimentKeys = {"experiment", "repeats", "matrix"};

std::vector<std::string> keys_under(const std::string& section) {
  std::vector<std::string> names;
  const std::string prefix = section.empty() ? "" : section + ".";
  for (const std::string& path : run_key_paths()) {
    if (path.rfind(prefix, 0) != 0) continue;
    const std::string rest = path.substr(prefix.size());
    const std::string name = rest.substr(0, rest.find('.'));
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  }
  if (section.empty()) {
    for (const std::string_view s : kSections) {
      if (std::find(names.begin(), names.end(), s) == names.end()) names.emplace_back(s);
    }
  }
  return names;
}

void unknown_key(const std::string& file, const YAML::Node& key, const std::string& section,
                 const std::vector<std::string>& extra, ConfigErrors& errors) {
  const std::string name = key.Scalar();
  if (section.empty() &&
      std::find(kExperimentKeys.begin(), kExperimentKeys.end(), name) != kExperimentKeys.end()) {
    errors.push_back(position(file, key) + "'" + name +
                     "' is only allowed in experiment files (use frameflow bench --config)");
    return;
  }
  std::vector<std::string> known = keys_under(section);
  known.insert(known.end(), extra.begin(), extra.end());
  errors.push_back(position(file, key) + "unknown key '" + name + "' (did you mean '" +
                   closest_flag(name, known) + "'?)");
}

// Total scalar bytes, counting every alias use; stops counting once above limit (SR-07).
std::uintmax_t expanded_size(const YAML::Node& node, std::uintmax_t limit, std::uintmax_t total) {
  if (total > limit) return total;
  if (node.IsScalar()) return total + node.Scalar().size();
  for (const auto& item : node) {
    if (node.IsMap()) {
      total = expanded_size(item.first, limit, total);
      total = expanded_size(item.second, limit, total);
    } else {
      total = expanded_size(item, limit, total);
    }
    if (total > limit) return total;
  }
  return total;
}

void read_section(const YAML::Node& section, const std::string& name, const std::string& file,
                  RawConfig& raw, ConfigErrors& errors) {
  if (!section.IsMap()) {
    errors.push_back(position(file, section) + "'" + name + "' must be a mapping, like " + name +
                     ": { type: ... }");
    return;
  }
  for (const auto& entry : section) {
    const std::string key = entry.first.Scalar();
    if (!set_run_key(raw, name + "." + key, entry.second, file, errors)) {
      unknown_key(file, entry.first, name, {}, errors);
    }
  }
}

}  // namespace

std::string position(const std::string& file, const YAML::Node& node) {
  const YAML::Mark mark = node.Mark();
  if (mark.is_null()) return file + ": ";
  return file + ":" + std::to_string(mark.line + 1) + ":" + std::to_string(mark.column + 1) + ": ";
}

std::optional<YAML::Node> load_yaml_file(const fs::path& path, ConfigErrors& errors) {
  const std::string file = path.string();
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    errors.push_back(file + ": cannot open config file: no such file");
    return std::nullopt;
  }
  if (fs::file_size(path, ec) > kMaxConfigBytes) {
    errors.push_back(file + ": config file is larger than 1 MB");
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in && !in.eof()) {
    errors.push_back(file + ": cannot read config file");
    return std::nullopt;
  }
  try {
    YAML::Node root = YAML::Load(text);
    if (expanded_size(root, kMaxConfigBytes, 0) > kMaxConfigBytes) {
      errors.push_back(file + ": config expands to more than 1 MB (aliases)");
      return std::nullopt;
    }
    return root;
  } catch (const YAML::Exception& e) {
    errors.push_back(file + ":" + std::to_string(e.mark.line + 1) + ":" +
                     std::to_string(e.mark.column + 1) + ": " + e.msg);
    return std::nullopt;
  }
}

const std::vector<std::string>& run_key_paths() {
  static const std::vector<std::string> paths = [] {
    std::vector<std::string> out;
    for (const auto& [path, field] : run_keys()) out.push_back(path);
    return out;
  }();
  return paths;
}

bool set_run_value(RawConfig& raw, const std::string& path, const std::string& value) {
  const auto& keys = run_keys();
  const auto it = std::find_if(keys.begin(), keys.end(),
                               [&](const auto& entry) { return entry.first == path; });
  if (it == keys.end()) return false;
  raw.*(it->second) = value;  // checked by validate(), like a flag (VR-18)
  return true;
}

bool set_run_key(RawConfig& raw, const std::string& path, const YAML::Node& value,
                 const std::string& file, ConfigErrors& errors) {
  const auto& paths = run_key_paths();
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) return false;
  if (!value.IsScalar()) {
    errors.push_back(position(file, value) + "'" + path + "' must be a single value");
    return true;
  }
  return set_run_value(raw, path, value.Scalar());
}

void read_run_keys(const YAML::Node& root, const std::string& file,
                   const std::vector<std::string>& extra_keys, RawConfig& raw,
                   ConfigErrors& errors) {
  if (!root.IsMap()) {
    errors.push_back(position(file, root) + "the file must be a mapping of keys to values");
    return;
  }
  for (const auto& entry : root) {
    const std::string key = entry.first.Scalar();
    if (std::find(extra_keys.begin(), extra_keys.end(), key) != extra_keys.end()) continue;
    if (std::find(kSections.begin(), kSections.end(), key) != kSections.end()) {
      read_section(entry.second, key, file, raw, errors);
    } else if (!set_run_key(raw, key, entry.second, file, errors)) {
      unknown_key(file, entry.first, "", extra_keys, errors);
    }
  }
}

ConfigFileResult load_run_config(const fs::path& path) {
  ConfigFileResult result;
  if (const std::optional<YAML::Node> root = load_yaml_file(path, result.errors)) {
    read_run_keys(*root, path.string(), {}, result.raw, result.errors);
  }
  return result;
}

RawConfig merge_raw(const RawConfig& base, const RawConfig& overrides) {
  RawConfig merged = base;
  for (const auto& [path, field] : run_keys()) {
    if (overrides.*field) merged.*field = overrides.*field;
  }
  return merged;
}

}  // namespace frameflow
