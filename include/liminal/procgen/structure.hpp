#pragma once
// Parameterized landmark generators. The app names a structure type, a size,
// a height and a theme; these functions build the actual boxy PS1 geometry
// plus per-piece collision AABBs (so doorways and the space under bridge
// decks stay genuinely open). All footprint/height math is deterministic in
// (size, height, seed) — the model never touches a vertex.

#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <liminal/render/mesh.hpp>

namespace liminal::procgen::structgen {

struct Built {
    MeshData mesh;
    std::vector<PartBox> boxes; // same space as the mesh's vertices
};

// Terrain height lookup for the world-frame builders (pillar feet).
using HeightFn = std::function<float(float, float)>;

// --- Local-frame builders ---------------------------------------------------
// Base on y=0, centered on x/z. Apps place them with a yaw snapped to
// 90 degrees so the rotated part boxes stay exact AABBs.
// `size` is the grammar's 1.0..3.9 footprint multiplier, `height` its 1..9.

Built building(float size, int height, unsigned int seed);    // hollow, door
Built barn(float size, int height, unsigned int seed);        // gabled, big door
Built tower(float size, int height, unsigned int seed);       // stacked insets
Built wallRun(float size, int height, unsigned int seed);     // wall + gap
Built bigArch(float size, int height, unsigned int seed);     // jambs + lintel
Built grandStairs(float size, int height, unsigned int seed); // steps + platform

// --- World-frame builders ---------------------------------------------------
// Vertices in world space (the game draws them with an identity model and
// uses `boxes` directly as world AABBs). `a`/`b` carry the desired deck-top
// height at each end in .y; `ground` supplies pillar feet.

// Stepped deck from a to b: axis-aligned plan-stepped segments whose rises
// are quantized so each step stays mountable (<= ~0.45). `severed` ends the
// deck mid-span with no support — the decay's work, never the model's.
Built bridge(const glm::vec3& a, const glm::vec3& b, float size,
             const HeightFn& ground, bool severed, unsigned int seed);

// Deck running from a shore point out along `dir` (cardinal, normalized)
// over water, posts every few meters and a pair at the far end.
Built pier(const glm::vec3& start, const glm::vec2& dir, float size,
           const HeightFn& ground, unsigned int seed);

} // namespace liminal::procgen::structgen
