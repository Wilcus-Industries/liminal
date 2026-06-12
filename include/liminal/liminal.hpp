// Liminal engine umbrella header.
// Engine subsystems (window, renderer, audio, ecs, scripting, inference)
// arrive in later phases; for now this only exposes version information.
#pragma once

#include <liminal/version.hpp>

namespace liminal {

// Returns the engine version string, e.g. "0.1.0".
const char* version() noexcept;

} // namespace liminal
