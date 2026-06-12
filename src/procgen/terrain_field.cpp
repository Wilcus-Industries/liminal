// terrain_field.cpp — terrain kind + seed -> walkable heightfield.
//
// Shape strategy: every kind computes a "wild" base height from value noise,
// then zone disks and connection-path ribbons pull the surface toward a flat
// target height with a smooth falloff. The flattening is what guarantees the
// requested layout is traversable — whatever the noise does, the places you
// must stand and the lines you must walk are tamed. The void kind inverts
// the logic: base is a deep floor and only the flattened parts exist as land.

#include <liminal/procgen/terrain.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>

namespace liminal::procgen {


namespace {

std::uint32_t hashU32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float hash01(std::uint32_t seed, int ix, int iz) {
    std::uint32_t h = hashU32(static_cast<std::uint32_t>(ix) * 0x9E3779B9U ^
                              hashU32(static_cast<std::uint32_t>(iz) * 0x85EBCA6BU ^ seed));
    return static_cast<float>(h & 0x00FFFFFFU) / 16777216.0f;
}

// Value noise on integer lattice `freq` cells wide, smooth-interpolated.
float valueNoise(std::uint32_t seed, float x, float z, float freq) {
    float fx = x * freq, fz = z * freq;
    int ix = static_cast<int>(std::floor(fx)), iz = static_cast<int>(std::floor(fz));
    float tx = fx - ix, tz = fz - iz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    float a = hash01(seed, ix, iz),     b = hash01(seed, ix + 1, iz);
    float c = hash01(seed, ix, iz + 1), d = hash01(seed, ix + 1, iz + 1);
    return (a + (b - a) * tx) * (1.0f - tz) + (c + (d - c) * tx) * tz;
}

float fbm(std::uint32_t seed, float x, float z, float freq) {
    return valueNoise(seed, x, z, freq) * 0.65f +
           valueNoise(seed ^ 0x5bd1e995U, x, z, freq * 2.7f) * 0.35f;
}

float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 0 beyond rFade, 1 inside rFlat.
float diskWeight(float d, float rFlat, float rFade) {
    return smoothstep01((rFade - d) / (rFade - rFlat));
}

float segmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, float& tOut) {
    glm::vec2 ab = b - a;
    float len2 = glm::dot(ab, ab);
    float t = (len2 > 1e-6f) ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    tOut = t;
    return glm::length(p - (a + ab * t));
}

} // namespace

TerrainField::TerrainField(const HeightField& hf, Kind kind)
    : m_kind(kind),
      m_half(hf.half),
      m_cell(hf.cell),
      m_n(hf.nodes),
      m_heights(hf.heights),
      m_hasWater(hf.hasWater),
      m_waterLevel(hf.waterLevel) {
    computeWaterDistance();
}

TerrainField::TerrainField(Kind kind, int seed, float halfSize,
                           const std::vector<glm::vec2>& zoneCenters,
                           const std::vector<glm::ivec2>& connections)
    : m_kind(kind), m_half(halfSize) {
    const std::uint32_t s = hashU32(static_cast<std::uint32_t>(seed) + 0x1234567U);
    m_n = static_cast<int>(2.0f * m_half / kCell) + 1;
    m_heights.resize(static_cast<size_t>(m_n) * m_n);

    switch (m_kind) {
        case Kind::Islands:  m_hasWater = true; m_waterLevel = -0.5f;  break;
        case Kind::Flooded:  m_hasWater = true; m_waterLevel = -0.25f; break;
        default:               m_hasWater = false; m_waterLevel = -1000.0f; break;
    }

    // Flat target height on the zone disks / path ribbons, per kind. Water
    // zones become low islands; flooded zones barely clear the surface.
    float zoneH = 0.0f;
    switch (m_kind) {
        case Kind::Islands:  zoneH = 0.45f; break;
        case Kind::Flooded:  zoneH = 0.15f; break;
        default:               zoneH = 0.0f;  break;
    }

    for (int iz = 0; iz < m_n; ++iz) {
        for (int ix = 0; ix < m_n; ++ix) {
            const float wx = -m_half + ix * kCell;
            const float wz = -m_half + iz * kCell;

            float base = 0.0f;
            switch (m_kind) {
                case Kind::Plane:
                    base = fbm(s, wx, wz, 0.020f) * 1.2f - 0.5f;
                    break;
                case Kind::Hills:
                    base = fbm(s, wx, wz, 0.014f) * 9.0f - 2.0f;
                    break;
                case Kind::Canyon: {
                    // Plateau with a deep winding trench carved through it.
                    base = 2.2f + fbm(s, wx, wz, 0.022f) * 1.6f;
                    const float trenchX = std::sin(wz * 0.025f + (s & 7) * 0.8f) * 22.0f;
                    const float d = std::fabs(wx - trenchX);
                    base -= smoothstep01((14.0f - d) / 14.0f) * 7.5f;
                    break;
                }
                case Kind::Islands:
                    // Seafloor, comfortably below the surface; islands come
                    // from the zone flattening below.
                    base = -2.3f + fbm(s, wx, wz, 0.020f) * 0.9f;
                    break;
                case Kind::Flooded:
                    // Hovers around the waterline: shallow pools everywhere,
                    // wading depth at worst so every walk line stays open.
                    base = fbm(s, wx, wz, 0.030f) * 1.5f - 0.95f;
                    break;
                case Kind::Void:
                    base = kVoidFloor;
                    break;
            }

            // Strongest flattening feature wins; its target height is used.
            float w = 0.0f, target = zoneH;
            const glm::vec2 p{wx, wz};
            for (const glm::vec2& c : zoneCenters) {
                const float zw = diskWeight(glm::length(p - c), 13.0f, 21.0f);
                if (zw > w) { w = zw; target = zoneH; }
            }
            for (const glm::ivec2& con : connections) {
                if (con.x < 0 || con.y < 0 ||
                    con.x >= static_cast<int>(zoneCenters.size()) ||
                    con.y >= static_cast<int>(zoneCenters.size()) || con.x == con.y) {
                    continue; // self-edges add no ribbon
                }
                float t = 0.0f;
                const float d = segmentDistance(p, zoneCenters[con.x], zoneCenters[con.y], t);
                const float pw = diskWeight(d, 3.2f, 7.5f);
                if (pw > w) { w = pw; target = zoneH; }
            }

            m_heights[index(ix, iz)] = base + (target - base) * w;
        }
    }

    computeWaterDistance();
}

void TerrainField::computeWaterDistance() {
    // Multi-source BFS from every underwater node; distance in world units.
    m_waterDist.assign(m_heights.size(), 9999.0f);
    if (!m_hasWater) return;
    std::queue<int> q;
    for (size_t i = 0; i < m_heights.size(); ++i) {
        if (m_heights[i] < m_waterLevel) {
            m_waterDist[i] = 0.0f;
            q.push(static_cast<int>(i));
        }
    }
    while (!q.empty()) {
        const int i = q.front();
        q.pop();
        const int ix = i % m_n, iz = i / m_n;
        const int nx[4] = {ix - 1, ix + 1, ix, ix};
        const int nz[4] = {iz, iz, iz - 1, iz + 1};
        for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || nx[k] >= m_n || nz[k] < 0 || nz[k] >= m_n) continue;
            const int j = index(nx[k], nz[k]);
            if (m_waterDist[j] > m_waterDist[i] + m_cell) {
                m_waterDist[j] = m_waterDist[i] + m_cell;
                q.push(j);
            }
        }
    }
}

glm::vec3 TerrainField::nodePos(int ix, int iz) const {
    return {-m_half + ix * m_cell, m_heights[index(ix, iz)], -m_half + iz * m_cell};
}

float TerrainField::height(float x, float z) const {
    const float gx = std::clamp((x + m_half) / m_cell, 0.0f, static_cast<float>(m_n - 1) - 1e-4f);
    const float gz = std::clamp((z + m_half) / m_cell, 0.0f, static_cast<float>(m_n - 1) - 1e-4f);
    const int ix = static_cast<int>(gx), iz = static_cast<int>(gz);
    const float tx = gx - ix, tz = gz - iz;
    const float a = m_heights[index(ix, iz)],     b = m_heights[index(ix + 1, iz)];
    const float c = m_heights[index(ix, iz + 1)], d = m_heights[index(ix + 1, iz + 1)];
    return (a + (b - a) * tx) * (1.0f - tz) + (c + (d - c) * tx) * tz;
}

float TerrainField::waterDistance(float x, float z) const {
    if (!m_hasWater) return 9999.0f;
    const float gx = std::clamp((x + m_half) / m_cell, 0.0f, static_cast<float>(m_n - 1));
    const float gz = std::clamp((z + m_half) / m_cell, 0.0f, static_cast<float>(m_n - 1));
    return m_waterDist[index(static_cast<int>(gx + 0.5f), static_cast<int>(gz + 0.5f))];
}

MeshData TerrainField::buildMesh(float uvPerUnit) const {
    return buildMeshMasked({}, uvPerUnit);
}

MeshData TerrainField::buildMeshMasked(const std::vector<std::uint8_t>& cells,
                                       float uvPerUnit) const {
    MeshData md;
    md.vertices.reserve(static_cast<size_t>(m_n - 1) * (m_n - 1) * 6 * 8);
    for (int iz = 0; iz < m_n - 1; ++iz) {
        for (int ix = 0; ix < m_n - 1; ++ix) {
            if (!cells.empty() &&
                !cells[static_cast<size_t>(iz) * (m_n - 1) + ix]) continue;
            const glm::vec3 a = nodePos(ix, iz);
            const glm::vec3 b = nodePos(ix + 1, iz);
            const glm::vec3 c = nodePos(ix + 1, iz + 1);
            const glm::vec3 d = nodePos(ix, iz + 1);
            auto uv = [uvPerUnit](const glm::vec3& v) {
                return glm::vec2{v.x * uvPerUnit, v.z * uvPerUnit};
            };
            // Winding so the face normals point up (+y is up, z grows toward d).
            md.addTriangle(a, c, b, uv(a), uv(c), uv(b));
            md.addTriangle(a, d, c, uv(a), uv(d), uv(c));
        }
    }
    return md;
}

MeshData TerrainField::buildWaterMesh(float uvTiles) const {
    MeshData md;
    if (!m_hasWater) return md;
    const float h = m_half, y = m_waterLevel;
    md.addQuad({-h, y, -h}, {-h, y, h}, {h, y, h}, {h, y, -h}, uvTiles);
    return md;
}

} // namespace liminal::procgen
