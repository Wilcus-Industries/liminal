#pragma once
// Plain-data render types shared between the renderer and its callers.
// The renderer knows nothing about the game; the app layer hands it
// DrawItems and mood-flavored settings — that's the whole contract.

#include <glm/glm.hpp>

namespace liminal {

class Mesh;
class Texture;

struct RenderSettings {
    // 400x300 + a finer snap grid: still unmistakably PS1, but a notch
    // clearer than the original 320x240 — the artifacts stay, the smear goes.
    int virtualW = 400;
    int virtualH = 300;

    bool vertexSnap = true;
    float snapScale = 1.4f;       // snap grid = virtual res * snapScale

    bool affineTextures = true;   // PS1 texture wobble

    glm::vec3 skyColor{0.42f, 0.39f, 0.47f};
    glm::vec3 fogColor{0.42f, 0.39f, 0.47f};
    float fogDensity = 0.046f;    // deliberately a touch too close

    // The two directions intentionally disagree: diffuse light comes from one
    // place, the darkening "shadow" term from another. Subtle and wrong.
    glm::vec3 lightDir{0.35f, 1.0f, 0.25f};
    glm::vec3 shadeDir{-0.55f, 0.8f, -0.15f};

    float fovDegrees = 70.0f;

    // 0..1 dream-decay value piped in from the app. Plumbed to the shaders;
    // the default retro pack uses it to unravel the world.
    float decayProgress = 0.0f;
};

struct DrawItem {
    const Mesh* mesh = nullptr;
    glm::mat4 model{1.0f};
    glm::vec3 color{1.0f};   // base color (object-local bottom)
    glm::vec3 color2{1.0f};  // top color; blended up the object's local height
    const Texture* texture = nullptr;
    // The one clean object: skips vertex snapping and affine warp. It sits
    // perfectly still while everything around it jitters.
    bool still = false;
    // Opt-in cutout for decals/figures; opaque draws leave it false.
    bool alphaTest = false;
    // Per-item multiplier on the global fog density. <1 lets a draw read
    // through fog the scene around it has already swallowed; 1 leaves it
    // fogged like everything else.
    float fogScale = 1.0f;
};

} // namespace liminal
