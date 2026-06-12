// Liminal engine umbrella header — includes every public subsystem.
// Apps that care about compile times can include the per-subsystem headers
// (<liminal/core/window.hpp>, <liminal/render/renderer.hpp>, ...) directly.
#pragma once

#include <liminal/version.hpp>

#include <liminal/core/assets.hpp>
#include <liminal/core/window.hpp>

#include <liminal/render/mesh.hpp>
#include <liminal/render/renderer.hpp>
#include <liminal/render/shader.hpp>
#include <liminal/render/texture.hpp>
#include <liminal/render/types.hpp>

#include <liminal/audio/audio.hpp>

#include <liminal/ui/imgui_layer.hpp>

namespace liminal {

// Returns the engine version string, e.g. "0.1.0".
const char* version() noexcept;

} // namespace liminal
