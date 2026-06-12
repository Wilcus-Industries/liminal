#pragma once
// Tiny deterministic xorshift32. Every procgen stage owns its own stream
// (seeded from the caller's master seed with a stage-local salt) so
// reordering one stage can never reshuffle another. Determinism contract:
// same seed, same sequence, on every platform.

#include <cstdint>

namespace liminal::procgen {

struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float next01() { return (next() & 0x00FFFFFFu) / 16777216.0f; }
    int range(int lo, int hi) { // inclusive
        return lo + static_cast<int>(next() % static_cast<std::uint32_t>(hi - lo + 1));
    }
};

} // namespace liminal::procgen
