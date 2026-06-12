// Liminal engine umbrella header — includes every public subsystem.
// Apps that care about compile times can include the per-subsystem headers
// (<liminal/core/window.hpp>, <liminal/render/renderer.hpp>, ...) directly.
#pragma once

#include <liminal/version.hpp>

#include <liminal/core/app.hpp>
#include <liminal/core/asset_cache.hpp>
#include <liminal/core/assets.hpp>
#include <liminal/core/window.hpp>

#include <liminal/scene/component_registry.hpp>
#include <liminal/scene/components.hpp>
#include <liminal/scene/entity.hpp>
#include <liminal/scene/scene.hpp>
#include <liminal/scene/serialize.hpp>

#include <liminal/render/mesh.hpp>
#include <liminal/render/renderer.hpp>
#include <liminal/render/shader.hpp>
#include <liminal/render/texture.hpp>
#include <liminal/render/types.hpp>

#include <liminal/audio/audio.hpp>

#include <liminal/procgen/layout_validator.hpp>
#include <liminal/procgen/rng.hpp>
#include <liminal/procgen/shape_grammar.hpp>
#include <liminal/procgen/structure.hpp>
#include <liminal/procgen/terrain.hpp>
#include <liminal/procgen/tileset.hpp>
#include <liminal/procgen/types.hpp>
#include <liminal/procgen/wfc.hpp>

#include <liminal/ui/imgui_layer.hpp>

#ifdef LIMINAL_WITH_INFERENCE
#include <liminal/inference/engine.hpp>
#endif

#ifdef LIMINAL_WITH_SCRIPTING
#include <liminal/script/script_host.hpp>
#endif

namespace liminal {

// Returns the engine version string, e.g. "0.1.0".
const char* version() noexcept;

} // namespace liminal
