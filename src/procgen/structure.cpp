// structure.cpp — boxy landmark geometry from (type, size, height, seed).
//
// Conventions:
//   - Everything is built from axis-aligned boxes (plus the barn's gable
//     slopes), so collision AABBs mirror the render geometry exactly.
//   - UVs are proportional to world extents (uvPerUnit) so the 64px theme
//     textures tile at a consistent chunky density on walls of any length.
//   - Walkable flags go on pieces whose top a player should be able to stand
//     on (decks, steps, floors, tower tiers); walls/jambs stay non-walkable
//     so their thin tops never read as ground.

#include <liminal/procgen/structure.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace liminal::procgen::structgen {

namespace {

constexpr float kUvPerUnit = 0.45f;

// Box with per-face proportional UVs. addQuad's single uvScale would stretch
// the texture across long decks; here every face maps texels per world unit.
void addBoxUV(MeshData& md, const glm::vec3& mn, const glm::vec3& mx) {
    const float sx = (mx.x - mn.x) * kUvPerUnit;
    const float sy = (mx.y - mn.y) * kUvPerUnit;
    const float sz = (mx.z - mn.z) * kUvPerUnit;
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                    float su, float sv) {
        md.addTriangle(a, b, c, {0, 0}, {su, 0}, {su, sv});
        md.addTriangle(a, c, d, {0, 0}, {su, sv}, {0, sv});
    };
    // +z / -z
    quad({mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}, sx, sy);
    quad({mx.x, mn.y, mn.z}, {mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, sx, sy);
    // +x / -x
    quad({mx.x, mn.y, mx.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, sz, sy);
    quad({mn.x, mn.y, mn.z}, {mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}, sz, sy);
    // +y / -y
    quad({mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z}, sx, sz);
    quad({mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}, sx, sz);
}

void box(Built& out, const glm::vec3& mn, const glm::vec3& mx, bool walkable = false) {
    addBoxUV(out.mesh, mn, mx);
    out.boxes.push_back(PartBox{mn, mx, walkable});
}

std::uint32_t hashU32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float hash01(unsigned int seed, unsigned int i) {
    return static_cast<float>(hashU32(i * 0x9E3779B9U ^ hashU32(seed)) & 0xFFFFFFU) / 16777216.0f;
}

// Door opening in a wall running along X at z in [z0,z1]: two flanking
// segments + a lintel above the gap. Gap is centered, `dw` wide, `dh` tall.
void wallWithDoor(Built& out, float x0, float x1, float z0, float z1,
                  float h, float dw, float dh) {
    const float cx = (x0 + x1) * 0.5f;
    const float g0 = cx - dw * 0.5f, g1 = cx + dw * 0.5f;
    box(out, {x0, 0.0f, z0}, {g0, h, z1});
    box(out, {g1, 0.0f, z0}, {x1, h, z1});
    if (dh < h) box(out, {g0, dh, z0}, {g1, h, z1});
}

} // namespace

Built building(float size, int height, unsigned int seed) {
    Built out;
    const float hw = 2.4f + size * 1.3f;             // half width (door wall on +z)
    const float hd = (2.0f + size * 1.1f);           // half depth
    const float h = 2.7f + height * 0.55f;           // wall height
    const float t = 0.35f;                            // wall thickness
    // Floor slab the player can stand on (slightly proud of the terrain).
    box(out, {-hw, 0.0f, -hd}, {hw, 0.25f, hd}, /*walkable=*/true);
    // Front wall (+z) with the door; door 1.7 wide, 2.3 tall.
    wallWithDoor(out, -hw, hw, hd - t, hd, h, 1.7f, 2.3f);
    // Back and side walls, solid.
    box(out, {-hw, 0.0f, -hd}, {hw, h, -hd + t});
    box(out, {-hw, 0.0f, -hd + t}, {-hw + t, h, hd - t});
    box(out, {hw - t, 0.0f, -hd + t}, {hw, h, hd - t});
    // Flat roof slab with a small overhang.
    box(out, {-hw - 0.3f, h, -hd - 0.3f}, {hw + 0.3f, h + 0.4f, hd + 0.3f});
    return out;
}

Built barn(float size, int height, unsigned int seed) {
    Built out;
    const float hw = 2.6f + size * 1.2f;   // gable ends face +/- z
    const float hd = 3.2f + size * 1.5f;
    const float h = 2.9f + height * 0.45f; // eave height
    const float ridge = h + 1.4f + size * 0.5f;
    const float t = 0.35f;
    box(out, {-hw, 0.0f, -hd}, {hw, 0.25f, hd}, /*walkable=*/true);
    // Big barn door (2.6 wide, 3.0 tall) in the +z gable wall.
    wallWithDoor(out, -hw, hw, hd - t, hd, h, 2.6f, std::min(3.0f, h - 0.3f));
    box(out, {-hw, 0.0f, -hd}, {hw, h, -hd + t});
    box(out, {-hw, 0.0f, -hd + t}, {-hw + t, h, hd - t});
    box(out, {hw - t, 0.0f, -hd + t}, {hw, h, hd - t});
    // Gabled roof: two slopes + gable triangles, ridge along z.
    const glm::vec3 e0{-hw - 0.3f, h, -hd - 0.3f}, e1{hw + 0.3f, h, -hd - 0.3f};
    const glm::vec3 e2{hw + 0.3f, h, hd + 0.3f},  e3{-hw - 0.3f, h, hd + 0.3f};
    const glm::vec3 r0{0.0f, ridge, -hd - 0.3f},  r1{0.0f, ridge, hd + 0.3f};
    const float su = (hw + 0.3f) * 1.6f * kUvPerUnit, sv = (hd + 0.3f) * 2.0f * kUvPerUnit;
    out.mesh.addTriangle(e3, r1, r0, {0, 0}, {su, 0}, {su, sv});
    out.mesh.addTriangle(e3, r0, e0, {0, 0}, {su, sv}, {0, sv});
    out.mesh.addTriangle(e1, r0, r1, {0, 0}, {su, 0}, {su, sv});
    out.mesh.addTriangle(e1, r1, e2, {0, 0}, {su, sv}, {0, sv});
    out.mesh.addTriangle(e0, r0, e1, {0, 0}, {su * 0.5f, sv}, {su, 0});
    out.mesh.addTriangle(e2, r1, e3, {0, 0}, {su * 0.5f, sv}, {su, 0});
    // One blocky AABB for the whole roof — it is far above walking height.
    out.boxes.push_back(PartBox{{-hw - 0.3f, h, -hd - 0.3f}, {hw + 0.3f, ridge, hd + 0.3f}});
    return out;
}

Built tower(float size, int height, unsigned int seed) {
    Built out;
    const int tiers = std::clamp(2 + height / 2, 2, 6);
    const float tierH = 2.0f + height * 0.35f;
    float half = 1.5f + size * 0.9f;
    float y = 0.0f;
    for (int i = 0; i < tiers; ++i) {
        box(out, {-half, y, -half}, {half, y + tierH, half}, /*walkable=*/true);
        y += tierH;
        half *= 0.82f;
    }
    // Cap: a squat pyramid as four triangles.
    const float apexY = y + 1.2f + size * 0.4f;
    const glm::vec3 c0{-half, y, -half}, c1{half, y, -half}, c2{half, y, half}, c3{-half, y, half};
    const glm::vec3 apex{0.0f, apexY, 0.0f};
    const float s = half * 2.0f * kUvPerUnit;
    out.mesh.addTriangle(c0, c1, apex, {0, 0}, {s, 0}, {s * 0.5f, s});
    out.mesh.addTriangle(c1, c2, apex, {0, 0}, {s, 0}, {s * 0.5f, s});
    out.mesh.addTriangle(c2, c3, apex, {0, 0}, {s, 0}, {s * 0.5f, s});
    out.mesh.addTriangle(c3, c0, apex, {0, 0}, {s, 0}, {s * 0.5f, s});
    out.boxes.push_back(PartBox{{-half, y, -half}, {half, apexY, half}});
    return out;
}

Built wallRun(float size, int height, unsigned int seed) {
    Built out;
    const float halfLen = 4.0f + size * 2.5f;
    const float h = 1.9f + height * 0.5f;
    const float t = 0.5f;
    // Gap position varies with seed so a row of walls doesn't read as copies.
    const float gapAt = (hash01(seed, 1) - 0.5f) * halfLen;
    const float g0 = gapAt - 0.95f, g1 = gapAt + 0.95f;
    box(out, {-halfLen, 0.0f, -t * 0.5f}, {g0, h, t * 0.5f});
    box(out, {g1, 0.0f, -t * 0.5f}, {halfLen, h, t * 0.5f});
    if (h > 2.8f) box(out, {g0, 2.4f, -t * 0.5f}, {g1, h, t * 0.5f}); // lintel
    return out;
}

Built bigArch(float size, int height, unsigned int seed) {
    Built out;
    const float halfSpan = 1.8f + size * 1.0f;
    const float h = 3.0f + height * 0.7f;
    const float jw = 0.7f, hd = 0.55f;
    box(out, {-halfSpan, 0.0f, -hd}, {-halfSpan + jw, h, hd});
    box(out, {halfSpan - jw, 0.0f, -hd}, {halfSpan, h, hd});
    box(out, {-halfSpan - 0.2f, h, -hd - 0.1f}, {halfSpan + 0.2f, h + 0.9f, hd + 0.1f});
    return out;
}

Built grandStairs(float size, int height, unsigned int seed) {
    Built out;
    const float rise = 0.42f; // < player step-up (0.5) so every step mounts
    const float run = 0.95f;
    const float halfW = 1.6f + size * 0.8f;
    const float top = 1.3f + height * 0.75f;
    const int steps = std::max(2, static_cast<int>(std::ceil(top / rise)));
    // Steps climb toward +z; each is a full-height box so the side profile
    // is solid and the collision matches the silhouette.
    for (int i = 0; i < steps; ++i) {
        box(out, {-halfW, 0.0f, i * run}, {halfW, (i + 1) * rise, (i + 1) * run},
            /*walkable=*/true);
    }
    // Top platform: somewhere to arrive at.
    const float pz = steps * run;
    box(out, {-halfW, 0.0f, pz}, {halfW, steps * rise, pz + 3.2f}, /*walkable=*/true);
    // Recenter on x/z like the other local builders (base already at y=0).
    const float cz = (pz + 3.2f) * 0.5f;
    for (size_t i = 0; i < out.mesh.vertices.size(); i += 8) out.mesh.vertices[i + 2] -= cz;
    for (PartBox& b : out.boxes) { b.mn.z -= cz; b.mx.z -= cz; }
    return out;
}

Built bridge(const glm::vec3& a, const glm::vec3& b, float size,
             const HeightFn& ground, bool severed, unsigned int seed) {
    Built out;
    const float halfW = 1.1f + size * 0.45f;
    const float deckT = 0.4f;
    const float rise = 0.42f;

    // Plan-stepped path: alternate axis-aligned moves in x and z so every
    // deck segment is an exact AABB even on a diagonal span.
    const float dx = b.x - a.x, dz = b.z - a.z;
    const int n = std::max(2, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dz)) / 3.0f)));
    const float sx = dx / n, sz = dz / n;

    // Deck-top profile: linear a.y -> b.y with a gentle hump, quantized to
    // `rise` so consecutive segments differ by at most one mountable step.
    const float hump = std::min(2.5f, std::hypot(dx, dz) * 0.06f);
    auto topAt = [&](float t) {
        const float raw = a.y + (b.y - a.y) * t + std::sin(t * 3.14159265f) * hump;
        return a.y + std::round((raw - a.y) / rise) * rise;
    };

    const int total = 2 * n;
    const int keep = severed ? std::max(2, static_cast<int>(total * (0.45f + hash01(seed, 3) * 0.2f)))
                             : total;
    float px = a.x, pz = a.z;
    int placed = 0;
    float prevTop = topAt(0.0f);
    for (int i = 0; i < n && placed < keep; ++i) {
        for (int axis = 0; axis < 2 && placed < keep; ++axis) {
            const float qx = axis == 0 ? px + sx : px;
            const float qz = axis == 0 ? pz : pz + sz;
            float top = topAt(static_cast<float>(placed + 1) / total);
            top = std::clamp(top, prevTop - rise, prevTop + rise); // one step max
            // Consecutive segments deliberately overlap (each AABB pads halfW on
            // both axes so corners fill), and the quantized profile makes their
            // tops land at exactly the same height — coplanar overlapping faces,
            // i.e. hard z-fighting. Alternate a sub-centimeter inset between
            // segments: every adjacent pair now differs on every shared plane,
            // invisible at the virtual resolution.
            const float eps = (placed & 1) ? 0.008f : 0.0f;
            const glm::vec3 mn{std::min(px, qx) - halfW + eps, top - deckT + eps,
                               std::min(pz, qz) - halfW + eps};
            const glm::vec3 mx{std::max(px, qx) + halfW - eps, top - eps,
                               std::max(pz, qz) + halfW - eps};
            box(out, mn, mx, /*walkable=*/true);
            // A pillar under every other segment, down to the ground/seafloor.
            // Pillar top stops just shy of the deck underside — flush would
            // z-fight with the deck's bottom face.
            if ((placed & 1) == 0 && !(severed && placed >= keep - 2)) {
                const float cx = (mn.x + mx.x) * 0.5f, cz2 = (mn.z + mx.z) * 0.5f;
                const float foot = ground(cx, cz2) - 0.6f;
                if (foot < top - deckT - 0.2f) {
                    box(out, {cx - 0.35f, foot, cz2 - 0.35f},
                        {cx + 0.35f, top - deckT - 0.02f, cz2 + 0.35f});
                }
            }
            prevTop = top;
            px = qx; pz = qz;
            ++placed;
        }
    }
    return out;
}

Built pier(const glm::vec3& start, const glm::vec2& dir, float size,
           const HeightFn& ground, unsigned int seed) {
    Built out;
    const float halfW = 0.9f + size * 0.35f;
    const float len = 9.0f + size * 5.0f;
    const float deckT = 0.35f;
    const float y = start.y;
    const glm::vec2 d = glm::normalize(dir);
    const glm::vec2 end = glm::vec2(start.x, start.z) + d * len;
    const glm::vec3 mn{std::min(start.x, end.x) - halfW, y - deckT, std::min(start.z, end.y) - halfW};
    const glm::vec3 mx{std::max(start.x, end.x) + halfW, y, std::max(start.z, end.y) + halfW};
    box(out, mn, mx, /*walkable=*/true);
    // Posts: pairs every ~4 units, plus the end pair, down into the water.
    const int posts = std::max(2, static_cast<int>(len / 4.0f));
    for (int i = 1; i <= posts; ++i) {
        const glm::vec2 p = glm::vec2(start.x, start.z) + d * (len * i / posts);
        const glm::vec2 side{-d.y, d.x};
        for (int s = -1; s <= 1; s += 2) {
            const glm::vec2 q = p + side * (halfW - 0.2f) * static_cast<float>(s);
            const float foot = ground(q.x, q.y) - 0.6f;
            box(out, {q.x - 0.18f, foot, q.y - 0.18f}, {q.x + 0.18f, y + 0.55f, q.y + 0.18f});
        }
    }
    return out;
}

} // namespace liminal::procgen::structgen
