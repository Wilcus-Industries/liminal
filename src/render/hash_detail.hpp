#pragma once
// Shared deterministic integer hash for procedural render assets.
//
// lowbias32 (Chris Wellons). Determinism matters here: the same dream seed must
// hallucinate the same procedural mesh lumps / noise texture every time it
// recurs, so mesh.cpp and texture.cpp share one bit mixer instead of keeping
// two copies that could silently drift. Their per-file hash01 folds differ and
// stay local.
//
// Internal header: lives next to the render sources, not under include/.

#include <cstdint>

namespace liminal::render_detail {

inline std::uint32_t hashU32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

} // namespace liminal::render_detail
