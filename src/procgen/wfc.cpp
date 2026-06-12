// wfc.cpp — simple tiled-model WFC over TileMask domains.
//
// Deliberately small: ~200 lines, no dependency. Entropy is the weighted
// Shannon measure with a deterministic noise tie-break; propagation is a
// work-queue over 4-neighbors. All randomness comes from one xorshift
// stream seeded by the caller, so the same seed always solves identically.

#include <liminal/procgen/wfc.hpp>

#include <liminal/procgen/rng.hpp>

#include <cmath>
#include <cstdio>
#include <deque>

namespace liminal::procgen {

namespace {

int popcount16(TileMask m) {
    int c = 0;
    while (m) {
        m &= static_cast<TileMask>(m - 1);
        ++c;
    }
    return c;
}

} // namespace

WfcResult solveWfc(const TileSet& ts, const std::vector<float>& weights,
                   const std::vector<TileMask>& initial, int n,
                   std::uint32_t seed, int maxRestarts) {
    WfcResult res;
    const int tileCount = ts.count();
    const size_t cells = static_cast<size_t>(n) * n;

    // Per-tile entropy terms, precomputed: w and w*log(w).
    std::vector<float> wlogw(static_cast<size_t>(tileCount), 0.0f);
    for (int i = 0; i < tileCount; ++i) {
        const float w = std::max(weights[static_cast<size_t>(i)], 1e-4f);
        wlogw[static_cast<size_t>(i)] = w * std::log(w);
    }
    auto weightOf = [&weights](int t) {
        return std::max(weights[static_cast<size_t>(t)], 1e-4f);
    };

    std::vector<TileMask> dom;
    for (int attempt = 0; attempt <= maxRestarts; ++attempt) {
        Rng rng(seed + static_cast<std::uint32_t>(attempt) * 0x9E3779B9u);
        dom = initial;
        bool contradiction = false;

        // Initial propagation: the pre-collapsed cells (water, void, stamped
        // footprints) must constrain their neighborhoods before any guess.
        std::deque<int> work;
        for (size_t i = 0; i < cells; ++i) {
            if (dom[i] == 0) { contradiction = true; break; }
            if (popcount16(dom[i]) < tileCount) work.push_back(static_cast<int>(i));
        }

        auto propagate = [&]() {
            while (!work.empty()) {
                const int i = work.front();
                work.pop_front();
                const int x = i % n, z = i / n;
                TileMask unionAdj = 0;
                for (int t = 0; t < tileCount; ++t) {
                    if (dom[i] & ts.maskOf(t)) unionAdj |= ts.adjacent(t);
                }
                const int nx[4] = {x - 1, x + 1, x, x};
                const int nz[4] = {z, z, z - 1, z + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || nx[k] >= n || nz[k] < 0 || nz[k] >= n) continue;
                    const int jdx = nz[k] * n + nx[k];
                    const TileMask before = dom[jdx];
                    const TileMask after = before & unionAdj;
                    if (after == before) continue;
                    dom[jdx] = after;
                    if (after == 0) return false;
                    work.push_back(jdx);
                }
            }
            return true;
        };

        if (contradiction || !propagate()) {
            ++res.restarts;
            continue;
        }

        // Collapse loop.
        bool failed = false;
        for (;;) {
            // Lowest-entropy undecided cell (deterministic noise tie-break).
            int best = -1;
            float bestE = 1e30f;
            for (size_t i = 0; i < cells; ++i) {
                const int options = popcount16(dom[i]);
                if (options <= 1) continue;
                float sumW = 0.0f, sumWLW = 0.0f;
                for (int t = 0; t < tileCount; ++t) {
                    if (dom[i] & ts.maskOf(t)) {
                        sumW += weightOf(t);
                        sumWLW += wlogw[static_cast<size_t>(t)];
                    }
                }
                const float entropy =
                    std::log(sumW) - sumWLW / sumW + rng.next01() * 1e-4f;
                if (entropy < bestE) {
                    bestE = entropy;
                    best = static_cast<int>(i);
                }
            }
            if (best < 0) break; // fully collapsed

            // Weighted pick from the domain.
            float total = 0.0f;
            for (int t = 0; t < tileCount; ++t) {
                if (dom[best] & ts.maskOf(t)) total += weightOf(t);
            }
            float r = rng.next01() * total;
            int pick = 0;
            for (int t = 0; t < tileCount; ++t) {
                if (!(dom[best] & ts.maskOf(t))) continue;
                r -= weightOf(t);
                pick = t;
                if (r <= 0.0f) break;
            }
            dom[best] = ts.maskOf(pick);
            work.push_back(best);
            if (!propagate()) {
                failed = true;
                break;
            }
        }

        if (!failed) {
            res.solved = true;
            break;
        }
        ++res.restarts;
    }

    // Emit tiles. On exhaustion, unresolved/contradicted cells read as tile 0
    // — the validator's repair pass treats the result as raw material.
    res.tiles.resize(cells, 0);
    for (size_t i = 0; i < cells; ++i) {
        const TileMask m = dom.empty() ? 0 : dom[i];
        if (popcount16(m) == 1) {
            for (int t = 0; t < tileCount; ++t) {
                if (m & ts.maskOf(t)) {
                    res.tiles[i] = static_cast<TileId>(t);
                    break;
                }
            }
        }
    }
    if (!res.solved) {
        std::fprintf(stderr, "[wfc] unsolved after %d restarts (tile-0 fill)\n",
                     res.restarts);
    }
    return res;
}

} // namespace liminal::procgen
