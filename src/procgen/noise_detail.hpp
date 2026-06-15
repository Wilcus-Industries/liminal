#pragma once
// Shared deterministic value-noise primitives for the procgen terrain stages.
//
// These are determinism-critical: terrain.cpp and terrain_field.cpp must hash
// and interpolate bit-identically (and structure.cpp shares the same bit
// mixer), so the implementations live here once instead of being copy-pasted
// per translation unit where they could silently drift. The determinism golden
// test (test_procgen_determinism) gates any change to these.
//
// Internal header: lives next to the procgen sources, not under include/.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace liminal::procgen::detail {

// lowbias32 integer hash (Chris Wellons) — the shared bit mixer.
inline std::uint32_t hashU32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// 2D lattice value in [0,1).
inline float hash01(std::uint32_t seed, int ix, int iz) {
    std::uint32_t h = hashU32(static_cast<std::uint32_t>(ix) * 0x9E3779B9U ^
                              hashU32(static_cast<std::uint32_t>(iz) * 0x85EBCA6BU ^ seed));
    return static_cast<float>(h & 0x00FFFFFFU) / 16777216.0f;
}

// Smooth-interpolated value noise on an integer lattice `freq` cells wide.
inline float valueNoise(std::uint32_t seed, float x, float z, float freq) {
    const float fx = x * freq, fz = z * freq;
    const int ix = static_cast<int>(std::floor(fx));
    const int iz = static_cast<int>(std::floor(fz));
    float tx = fx - ix, tz = fz - iz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float a = hash01(seed, ix, iz),     b = hash01(seed, ix + 1, iz);
    const float c = hash01(seed, ix, iz + 1), d = hash01(seed, ix + 1, iz + 1);
    return (a + (b - a) * tx) * (1.0f - tz) + (c + (d - c) * tx) * tz;
}

inline float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace liminal::procgen::detail
