// display.hpp
// Responsibility: what the user's screen can do: whether a graphical display is available for
// `--preview` (ER-06: a non-empty DISPLAY or WAYLAND_DISPLAY), and whether stderr is a terminal
// (`--progress` rewrites its line in place only then). Platform calls live here (RR-12).
// Thread-safety: pure function plus a getenv wrapper; called on the main thread before any
// pipeline thread starts.
#pragma once

namespace frameflow {

bool display_available(const char* display, const char* wayland_display);

// Reads DISPLAY and WAYLAND_DISPLAY from the environment.
bool display_available();

bool stderr_is_terminal();

}  // namespace frameflow
