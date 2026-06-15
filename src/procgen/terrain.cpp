// terrain.cpp — TerrainParams -> quantized heightfield + water/void mask.
//
// Same value-noise family as the legacy TerrainField, but grid-aligned to
// the WFC tiles and quantized to coarse steps: chunky low-poly terrain, not
// smooth hills.

#include <liminal/procgen/terrain.hpp>

#include "noise_detail.hpp"

#include <algorithm>
#include <cmath>

namespace liminal::procgen {


namespace {

using detail::hash01;
using detail::hashU32;
using detail::smoothstep01;
using detail::valueNoise;

float fbm(std::uint32_t seed, float x, float z, float freq, int octaves) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, f = freq;
    std::uint32_t s = seed;
    for (int o = 0; o < octaves; ++o) {
        sum += valueNoise(s, x, z, f) * amp;
        norm += amp;
        amp *= 0.55f;
        f *= 2.3f;
        s ^= 0x5bd1e995U + (s << 6);
    }
    return sum / norm; // 0..1-ish
}

// The chunk size of the world. Bilinear sampling between nodes still slopes,
// but the surface visibly terraces — honest PS1 ground.
constexpr float kQuant = 0.5f;

float quantize(float h) { return std::round(h / kQuant) * kQuant; }

// Water level as a height-distribution quantile: "some" submerges the dips,
// "lots" drowns most of the field. Nudged off the quantization lattice so a
// tile is never knife-edge "exactly at" the waterline.
float quantileLevel(const std::vector<float>& heights, float q) {
    std::vector<float> sorted = heights;
    std::sort(sorted.begin(), sorted.end());
    const size_t i = static_cast<size_t>(
        std::clamp(q, 0.0f, 1.0f) * static_cast<float>(sorted.size() - 1));
    return sorted[i] + kQuant * 0.27f;
}

} // namespace

HeightField generateTerrain(const TerrainParams& p) {
    HeightField hf;
    hf.nodes = p.gridN + 1;
    hf.cell = p.tileSize;
    hf.half = p.gridN * p.tileSize * 0.5f;
    hf.heights.resize(static_cast<size_t>(hf.nodes) * hf.nodes);

    const std::uint32_t s = hashU32(p.seed ^ 0x7E22A1Du);

    for (int iz = 0; iz < hf.nodes; ++iz) {
        for (int ix = 0; ix < hf.nodes; ++ix) {
            const float wx = -hf.half + ix * hf.cell;
            const float wz = -hf.half + iz * hf.cell;

            float h = 0.0f;
            switch (p.kind) {
                case TerrainParams::Kind::Plane:
                    h = fbm(s, wx, wz, 0.018f, 2) * 1.6f - 0.7f;
                    break;
                case TerrainParams::Kind::Hills:
                    h = fbm(s, wx, wz, 0.012f, 3) * 9.0f - 2.5f;
                    break;
                case TerrainParams::Kind::Canyon: {
                    // Plateau with a winding trench. Ridged second octave
                    // keeps the walls craggy.
                    h = 2.4f + fbm(s, wx, wz, 0.020f, 2) * 1.8f;
                    const float ridge =
                        1.0f - std::fabs(fbm(s ^ 0x9D2CU, wx, wz, 0.016f, 2) * 2.0f - 1.0f);
                    h += ridge * 1.2f;
                    const float trenchX =
                        std::sin(wz * 0.022f + (s & 7) * 0.8f) * hf.half * 0.2f;
                    const float d = std::fabs(wx - trenchX);
                    h -= smoothstep01((16.0f - d) / 16.0f) * 8.0f;
                    break;
                }
                case TerrainParams::Kind::Islands:
                    // Island field: seafloor with noise bumps that crest the
                    // future waterline here and there.
                    h = -2.6f + fbm(s, wx, wz, 0.020f, 3) * 5.2f;
                    break;
                case TerrainParams::Kind::Flooded:
                    // Hovers around zero: shallow pools in every dip.
                    h = fbm(s, wx, wz, 0.028f, 2) * 1.8f - 0.9f;
                    break;
                case TerrainParams::Kind::Void: {
                    // Land plateaus over nothing. The mask turns everything
                    // below the rim into the void tile.
                    const float m = fbm(s, wx, wz, 0.014f, 2);
                    h = (m > 0.46f)
                            ? fbm(s ^ 0x33CU, wx, wz, 0.03f, 2) * 1.0f - 0.4f
                            : -28.0f;
                    break;
                }
            }
            hf.heights[hf.index(ix, iz)] = quantize(h);
        }
    }

    // Water table.
    switch (p.water) {
        case TerrainParams::Water::None:
            hf.hasWater = false;
            hf.waterLevel = -1000.0f;
            break;
        case TerrainParams::Water::Some:
            hf.hasWater = true;
            hf.waterLevel = quantileLevel(hf.heights, 0.22f);
            break;
        case TerrainParams::Water::Lots:
            hf.hasWater = true;
            hf.waterLevel = quantileLevel(hf.heights, 0.55f);
            break;
    }
    if (p.kind == TerrainParams::Kind::Void) {
        // Void's "low" is nothing, not sea. Water only on the plateaus, and
        // only if there are dips up there — quantile over land nodes alone.
        if (hf.hasWater) {
            std::vector<float> land;
            for (float h : hf.heights)
                if (h > -20.0f) land.push_back(h);
            hf.hasWater = land.size() > 16;
            if (hf.hasWater)
                hf.waterLevel = quantileLevel(land, p.water == TerrainParams::Water::Lots ? 0.4f : 0.15f);
        }
    }
    return hf;
}

std::vector<TileMask> terrainMask(const HeightField& hf, const TerrainParams& p,
                                  const TileSet& ts) {
    const int n = hf.nodes - 1; // tiles per side
    const TileMask all = ts.allMask();
    const TileMask voidM = ts.roleMask("void");
    const TileMask waterM = ts.roleMask("water");
    const TileMask deckM = ts.roleMask("deck");
    const TileMask pierM = ts.roleMask("pier");
    const TileMask shoreM = ts.roleMask("shore");
    std::vector<TileMask> mask(static_cast<size_t>(n) * n, all);

    auto idx = [n](int x, int z) { return static_cast<size_t>(z) * n + x; };

    // Pass 1: water / void straight from the heights.
    std::vector<std::uint8_t> wet(mask.size(), 0);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const float h = hf.tileHeight(x, z);
            if (p.kind == TerrainParams::Kind::Void && h < -20.0f) {
                mask[idx(x, z)] = voidM;
            } else if (hf.hasWater && h < hf.waterLevel) {
                mask[idx(x, z)] = waterM | deckM | pierM;
                wet[idx(x, z)] = 1;
            }
        }
    }

    // Pass 2: land tiles 4-adjacent to a wet tile become the coastline.
    const TileMask landMask =
        all & static_cast<TileMask>(~(waterM | voidM | deckM | pierM | shoreM));
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            if (mask[idx(x, z)] != all) continue; // already wet/void
            bool coast = false;
            if (x > 0 && wet[idx(x - 1, z)]) coast = true;
            if (x + 1 < n && wet[idx(x + 1, z)]) coast = true;
            if (z > 0 && wet[idx(x, z - 1)]) coast = true;
            if (z + 1 < n && wet[idx(x, z + 1)]) coast = true;
            mask[idx(x, z)] = coast ? (shoreM | pierM) : landMask;
        }
    }
    return mask;
}

} // namespace liminal::procgen
