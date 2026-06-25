#include <liminal/scene/raycast.hpp>

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include <liminal/core/asset_cache.hpp>
#include <liminal/render/mesh.hpp>
#include <liminal/scene/components.hpp>
#include <liminal/scene/scene.hpp>

namespace liminal {

std::optional<RayHit> raycastScene(Scene& scene, AssetCache& assets,
                                   const glm::vec3& origin,
                                   const glm::vec3& dir, float maxDist) {
    const float cap = maxDist <= 0.0f ? 1e30f : maxDist;
    std::optional<RayHit> best;
    float bestAlong = 1e30f;

    scene.each<Transform>([&](Entity e, Transform& t) {
        // Resolve the local-space AABB: Collider (when non-degenerate) wins,
        // otherwise the resolved mesh's local bounds; skip if neither exists.
        glm::vec3 lo, hi;
        bool haveBox = false;
        if (e.has<Collider>()) {
            const Collider& c = e.get<Collider>();
            if (c.halfExtents.x != 0.0f || c.halfExtents.y != 0.0f ||
                c.halfExtents.z != 0.0f) {
                lo = c.center - c.halfExtents;
                hi = c.center + c.halfExtents;
                haveBox = true;
            }
        }
        if (!haveBox && e.has<MeshRenderer>()) {
            const Mesh* mesh = assets.mesh(e.get<MeshRenderer>().meshAsset);
            if (mesh) {
                lo = mesh->localMin;
                hi = mesh->localMax;
                haveBox = true;
            }
        }
        if (!haveBox) return;

        const glm::mat4 model = t.matrix();
        const glm::mat4 invModel = glm::inverse(model);
        const glm::vec3 lo_ray(invModel * glm::vec4(origin, 1.0f));
        const glm::vec3 ld(invModel * glm::vec4(dir, 0.0f));

        float tMin = 0.0f, tMax = 1e30f;
        int hitAxis = -1;
        float hitSign = 0.0f;
        bool hit = true;
        for (int i = 0; i < 3; ++i) {
            if (std::abs(ld[i]) < 1e-8f) {
                if (lo_ray[i] < lo[i] || lo_ray[i] > hi[i]) {
                    hit = false;
                    break;
                }
                continue;
            }
            float t0 = (lo[i] - lo_ray[i]) / ld[i];
            float t1 = (hi[i] - lo_ray[i]) / ld[i];
            float sign = -1.0f; // entering the low face
            if (t0 > t1) {
                std::swap(t0, t1);
                sign = 1.0f;
            }
            if (t0 > tMin) {
                tMin = t0;
                hitAxis = i;
                hitSign = sign;
            }
            tMax = std::min(tMax, t1);
            if (tMin > tMax) {
                hit = false;
                break;
            }
        }
        if (!hit) return;

        const glm::vec3 worldHit(model * glm::vec4(lo_ray + ld * tMin, 1.0f));
        const float along = glm::dot(worldHit - origin, dir);
        if (along <= 1e-4f || along > cap) return;
        if (along >= bestAlong) return;

        glm::vec3 wn(0.0f);
        if (hitAxis >= 0) {
            glm::vec3 ln(0.0f);
            ln[hitAxis] = hitSign;
            wn = glm::normalize(glm::mat3(glm::transpose(invModel)) * ln);
        }

        bestAlong = along;
        best = RayHit{e.handle(), worldHit, wn, along};
    });

    return best;
}

} // namespace liminal
