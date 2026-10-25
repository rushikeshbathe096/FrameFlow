// policy_factory.hpp
// Responsibility: creates an overload policy from its CLI name (FR-POL-01, VR-07). Each policy is
// registered with one line in policy_factory.cpp (RR-05). Names are case-sensitive.
// Thread-safety: the registry is immutable; both functions are safe from any thread.
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "frameflow/policy/overload_policy.hpp"

namespace frameflow {

// Throws std::invalid_argument for an unknown name; the message lists the valid names.
std::unique_ptr<OverloadPolicy> make_policy(std::string_view name);

// Registered names in display order (blocking, drop-oldest, drop-newest, ...).
std::vector<std::string_view> policy_names();

}  // namespace frameflow
