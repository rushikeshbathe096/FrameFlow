#include <stdexcept>
#include <string>
#include <utility>

#include "frameflow/processor/image_processors.hpp"
#include "frameflow/processor/processor.hpp"

namespace frameflow {

namespace {

using Creator = std::unique_ptr<IProcessor> (*)(int kernel);

template <typename Processor>
std::unique_ptr<IProcessor> make(int /*kernel*/) {
  return std::make_unique<Processor>();
}

template <>
std::unique_ptr<IProcessor> make<BlurProcessor>(int kernel) {
  return std::make_unique<BlurProcessor>(kernel);
}

// One line per processor (RR-06). A vector keeps the display order stable.
const std::vector<std::pair<std::string_view, Creator>>& registry() {
  static const std::vector<std::pair<std::string_view, Creator>> entries = {
      {"none", &make<NoneProcessor>},
      {"grayscale", &make<GrayscaleProcessor>},
      {"blur", &make<BlurProcessor>},
      {"canny", &make<CannyProcessor>},
  };
  return entries;
}

}  // namespace

std::unique_ptr<IProcessor> make_processor(std::string_view name, int kernel) {
  for (const auto& [registered, create] : registry()) {
    if (registered == name) return create(kernel);
  }
  std::string valid;
  for (std::string_view registered : processor_names()) {
    valid += (valid.empty() ? "" : ", ") + std::string(registered);
  }
  throw std::invalid_argument("unknown processor '" + std::string(name) + "' (valid: " + valid +
                              ")");
}

std::vector<std::string_view> processor_names() {
  std::vector<std::string_view> names;
  for (const auto& entry : registry()) names.push_back(entry.first);
  return names;
}

}  // namespace frameflow
