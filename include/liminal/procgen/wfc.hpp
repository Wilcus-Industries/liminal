#pragma once
// Stage 3: macro spatial layout via simple tiled-model Wave Function
// Collapse. Cells hold TileMask domains; collapse picks the lowest-entropy
// cell, fixes a weighted-random tile, and propagates the adjacency rules
// until quiescent. Contradictions restart the whole solve with seed+1.
//
// The adjacency table lives in the TileSet (data, with a compiled-in
// fallback). `weights` is a working copy of the TileSet's base weights that
// spec semantics may have multiplied — one float per tile id.

#include <vector>

#include <liminal/procgen/tileset.hpp>
#include <liminal/procgen/types.hpp>

namespace liminal::procgen {

struct WfcResult {
    bool solved = false;
    int restarts = 0;   // contradiction restarts consumed
    std::vector<TileId> tiles; // n*n on success
};

// `initial` is the per-cell domain restriction from upstream stages
// (terrain water/void, stamped footprints); ts.allMask() = unconstrained.
// maxRestarts bounds the contradiction retries; on exhaustion `solved` is
// false and tiles holds the best-effort last attempt with contradictions
// resolved to tile id 0 (the validator treats that as repair input).
WfcResult solveWfc(const TileSet& ts, const std::vector<float>& weights,
                   const std::vector<TileMask>& initial, int n,
                   std::uint32_t seed, int maxRestarts = 24);

} // namespace liminal::procgen
