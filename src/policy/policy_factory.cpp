#include "frameflow/policy/policy_factory.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "frameflow/policy/blocking_policy.hpp"
#include "frameflow/policy/drop_newest_policy.hpp"
#include "frameflow/policy/drop_oldest_policy.hpp"

namespace frameflow {

namespace {

using Creator = std::unique_ptr<OverloadPolicy> (*)();

template <typename Policy>
std::unique_ptr<OverloadPolicy> make() {
  return std::make_unique<Policy>();
}

// One line per policy (RR-05). A vector keeps the display order stable.
const std::vector<std::pair<std::string_view, Creator>>& registry() {
  static const std::vector<std::pair<std::string_view, Creator>> entries = {
      {"blocking", &make<BlockingPolicy>},
      {"drop-oldest", &make<DropOldestPolicy>},
      {"drop-newest", &make<DropNewestPolicy>},
  };
  return entries;
}

}  // namespace

std::unique_ptr<OverloadPolicy> make_policy(std::string_view name) {
  for (const auto& [registered, create] : registry()) {
    if (registered == name) return create();
  }
  std::string valid;
  for (std::string_view registered : policy_names()) {
    valid += (valid.empty() ? "" : ", ") + std::string(registered);
  }
  throw std::invalid_argument("unknown policy '" + std::string(name) + "' (valid: " + valid + ")");
}

std::vector<std::string_view> policy_names() {
  std::vector<std::string_view> names;
  for (const auto& entry : registry()) names.push_back(entry.first);
  return names;
}

}  // namespace frameflow
