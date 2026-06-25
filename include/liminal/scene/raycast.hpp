#pragma once
// Ray-vs-scene query shared by the editor pick and lm.physics.raycast.
#include <optional>

#include <glm/glm.hpp>
#include <entt/entity/entity.hpp> // entt::entity

namespace liminal {

class Scene;
class AssetCache;

struct RayHit {
    entt::entity entity = entt::null;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    float distance = 0.0f; // along `dir` from `origin` (world units)
};

// Nearest entity whose axis-aligned box the ray hits within (0, maxDist].
// The box is the entity's Collider (center +/- halfExtents) when present and
// non-degenerate, otherwise the resolved mesh's local bounds. Entities with
// neither a Collider nor a resolvable mesh are skipped. `dir` should be
// normalized (distance is measured along it). nullopt on no hit.
std::optional<RayHit> raycastScene(Scene& scene, AssetCache& assets,
                                   const glm::vec3& origin,
                                   const glm::vec3& dir, float maxDist);

} // namespace liminal
