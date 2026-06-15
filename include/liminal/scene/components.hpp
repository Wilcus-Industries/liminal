#pragma once
// Built-in scene components: plain structs, no behavior. The Scene is a flat
// list of entities (no parenting/hierarchy this phase — a Transform is always
// world-space). Serialization and editor inspection go through the
// ComponentRegistry, so the structs themselves stay dependency-free.

#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace liminal {

// Human-readable handle; Scene::create() adds one automatically.
struct Name {
    std::string value;
};

// World-space placement. rotationEuler is in DEGREES, applied yaw(Y) ->
// pitch(X) -> roll(Z).
struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotationEuler{0.0f};
    glm::vec3 scale{1.0f};

    glm::mat4 matrix() const; // T * Ry * Rx * Rz * S
};

// Renders a mesh through the App's built-in system. Assets are referenced by
// string and resolved lazily through AssetCache each frame:
//   "builtin:box" | "builtin:pyramid" | "builtin:pillar" | "builtin:arch" |
//   "builtin:stair" | "builtin:plane" | "builtin:quad"
//   seeded primitives:  "builtin:blob:42", "builtin:tree:7",
//                       "builtin:rock:3",  "builtin:crystal:5"
// Unknown names warn once and the entity is skipped — a scene with missing
// assets still loads and runs. Strings (not GPU handles) keep serialization
// trivial.
struct MeshRenderer {
    std::string meshAsset = "builtin:box";
    // rgb tints the mesh (fed to DrawItem color+color2); alpha is currently
    // ignored — the retro renderer does no blending.
    glm::vec4 color{1.0f};
    // "" = untextured. "builtin:checker" / "builtin:grid" / "builtin:noise" /
    // "builtin:white" / material names ("builtin:wood", ...), or a file path
    // resolved through Assets::resolve at first use.
    std::string textureAsset;
};

// The entity's Transform supplies the view (view = inverse(transform)).
// fovDeg feeds RenderSettings.fovDegrees. nearZ/farZ are advisory: the retro
// Renderer fixes its planes at 0.1/220 internally; they're kept here so the
// component survives a future renderer that honors them.
struct Camera {
    float fovDeg = 70.0f;
    float nearZ = 0.1f;
    float farZ = 220.0f;
    bool primary = true;
    // Selectable shader pack name (see liminal::shaderCatalog()). "native" is
    // the built-in PS1 look.
    std::string shaderName = "native";
};

// liminal::Audio is a single procedural-DSP voice bank, not a mixer of
// positional sources — so this component is honest about that: it is a
// contribution to the global AudioParams. The App applies the first enabled
// AudioSource's gain/enabled to Audio::params each frame. Advisory beyond
// that.
struct AudioSource {
    float gain = 0.14f;
    bool enabled = true;
};

// Advisory only: the retro renderer has no light system (its lightDir/shadeDir
// live in RenderSettings). Carried for serialization and future passes —
// useful today as a fog/tint hint for app code.
struct Light {
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
};

// Behavior via Lua: an entity may run any number of scripts, each its own
// path. Stored as a list so a single entity can compose multiple behaviors;
// legacy single-`path` JSON still loads (see registerScript).
struct Script {
    std::vector<std::string> paths;
};

} // namespace liminal
