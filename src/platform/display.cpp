#include "frameflow/platform/display.hpp"

#include <unistd.h>

#include <cstdlib>

namespace frameflow {

bool display_available(const char* display, const char* wayland_display) {
  const auto set = [](const char* value) { return value != nullptr && *value != '\0'; };
  return set(display) || set(wayland_display);
}

bool display_available() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): main thread, before any pipeline thread; no setenv
  return display_available(std::getenv("DISPLAY"), std::getenv("WAYLAND_DISPLAY"));
}

bool stderr_is_terminal() { return ::isatty(STDERR_FILENO) == 1; }

}  // namespace frameflow
