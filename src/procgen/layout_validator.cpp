// layout_validator.cpp — reachability guarantees by flood fill + carving.
//
// Walkability model: TileSet-walkable tiles connect 4-adjacent. A building's
// floor cells (the gated role) join the network only through its door, which
// is chosen here (after the solve) as the edge-ring cell with the
// friendliest outside neighbor. Repair never deletes a building and never
// fails: any gap can be carved as path over land and deck over water/void.

#include <liminal/procgen/layout_validator.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>

namespace liminal::procgen {

namespace {

constexpr int kDX[4] = {-1, 1, 0, 0};
constexpr int kDZ[4] = {0, 0, -1, 1};

// The validator's working vocabulary, resolved from the TileSet's roles
// once per run.
struct Roles {
    int ground = -1, path = -1, water = -1, deck = -1, pier = -1;
    int floor = -1, edge = -1, plaza = -1, voidT = -1;
    explicit Roles(const TileSet& ts)
        : ground(ts.roleId("ground")),
          path(ts.roleId("path")),
          water(ts.roleId("water")),
          deck(ts.roleId("deck")),
          pier(ts.roleId("pier")),
          floor(ts.roleId("floor")),
          edge(ts.roleId("edge")),
          plaza(ts.roleId("plaza")),
          voidT(ts.roleId("void")) {}
};

struct FloodContext {
    const TileGrid& grid;
    const TileSet& ts;
    const Roles& roles;
    const std::vector<FootprintPlan>& plans;
    // Per tile: index into plans + 1 for floor/edge cells, 0 = free world.
    std::vector<int> owner;
    // Per tile: 1 if this is some plan's door cell.
    std::vector<std::uint8_t> isDoor;

    FloodContext(const TileGrid& g, const TileSet& tileset, const Roles& r,
                 const std::vector<FootprintPlan>& p)
        : grid(g), ts(tileset), roles(r), plans(p) {
        owner.assign(static_cast<size_t>(g.n) * g.n, 0);
        isDoor.assign(owner.size(), 0);
        for (size_t i = 0; i < plans.size(); ++i) {
            const FootprintPlan& fp = plans[i];
            for (int z = fp.z0; z < fp.z0 + fp.d; ++z) {
                for (int x = fp.x0; x < fp.x0 + fp.w; ++x) {
                    if (g.inBounds(x, z))
                        owner[static_cast<size_t>(z) * g.n + x] = static_cast<int>(i) + 1;
                }
            }
            if (fp.doorX >= 0 && g.inBounds(fp.doorX, fp.doorZ))
                isDoor[static_cast<size_t>(fp.doorZ) * g.n + fp.doorX] = 1;
        }
    }

    // May the flood step from tile a to 4-adjacent tile b?
    bool passable(int x, int z) const {
        const int t = grid.at(x, z);
        if (ts.walkable(t)) return true;
        const size_t i = static_cast<size_t>(z) * grid.n + x;
        // Inside a building: floor is walkable territory (gated), the wall
        // ring is not — except the door cell, which is the threshold.
        if (ts.gated(t)) return true;
        if (t == roles.edge && isDoor[i]) return true;
        return false;
    }
};

std::vector<std::uint8_t> floodFrom(const FloodContext& ctx, int sx, int sz) {
    const int n = ctx.grid.n;
    std::vector<std::uint8_t> seen(static_cast<size_t>(n) * n, 0);
    if (!ctx.grid.inBounds(sx, sz) || !ctx.passable(sx, sz)) return seen;
    std::deque<int> q;
    seen[static_cast<size_t>(sz) * n + sx] = 1;
    q.push_back(sz * n + sx);
    while (!q.empty()) {
        const int i = q.front();
        q.pop_front();
        const int x = i % n, z = i / n;
        for (int k = 0; k < 4; ++k) {
            const int nx = x + kDX[k], nz = z + kDZ[k];
            if (!ctx.grid.inBounds(nx, nz)) continue;
            const size_t j = static_cast<size_t>(nz) * n + nx;
            if (seen[j] || !ctx.passable(nx, nz)) continue;
            seen[j] = 1;
            q.push_back(static_cast<int>(j));
        }
    }
    return seen;
}

// Carve a straight-ish L path from (from) to (to): land becomes path,
// water/void becomes deck, a building ring in the way becomes path
// (a wall with a hole in it — dreams have those). Floor cells are stepped
// around by carving the ring instead; good enough at this resolution.
int carve(TileGrid& grid, const TileSet& ts, const Roles& roles, int fx,
          int fz, int tx, int tz) {
    int repairs = 0;
    auto carveCell = [&](int x, int z) {
        if (!grid.inBounds(x, z)) return;
        const int t = grid.at(x, z);
        if (ts.walkable(t)) return;
        int next = roles.path;
        if (t == roles.water || t == roles.voidT) next = roles.deck;
        grid.set(x, z, static_cast<TileId>(next));
        ++repairs;
    };
    int x = fx, z = fz;
    while (x != tx) {
        x += (tx > x) ? 1 : -1;
        carveCell(x, z);
    }
    while (z != tz) {
        z += (tz > z) ? 1 : -1;
        carveCell(x, z);
    }
    return repairs;
}

// Nearest reached tile to a target, by squared distance (small grids; linear
// scan is fine and deterministic).
bool nearestReached(const std::vector<std::uint8_t>& seen, int n, int tx,
                    int tz, int& outX, int& outZ) {
    long best = -1;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            if (!seen[static_cast<size_t>(z) * n + x]) continue;
            const long dx = x - tx, dz = z - tz;
            const long d = dx * dx + dz * dz;
            if (best < 0 || d < best) {
                best = d;
                outX = x;
                outZ = z;
            }
        }
    }
    return best >= 0;
}

} // namespace

ValidateOutcome validateAndRepair(TileGrid& grid, const HeightField& terrain,
                                  const std::vector<glm::ivec2>& sites,
                                  std::vector<FootprintPlan>& footprints,
                                  const TileSet& ts) {
    ValidateOutcome out;
    const int n = grid.n;
    const Roles roles(ts);
    auto at = [&grid](int x, int z) { return static_cast<int>(grid.at(x, z)); };
    (void)terrain;

    // --- doors: pick per footprint, post-solve -----------------------------
    // Best ring cell = one with a walkable 4-neighbor outside the rect;
    // prefer path/plaza neighbors. No candidate -> take the rect's mid-south
    // ring cell and let the carve below build a path to it.
    for (FootprintPlan& fp : footprints) {
        int bestX = -1, bestZ = -1, bestScore = -1;
        // Corner ring cells are off limits: the grammar maps a corner door to
        // doorAlong ~ 0, so half the opening clamps away and the perpendicular
        // wall run crosses the hole — an unenterable slot.
        const bool avoidCorners = fp.w >= 3 && fp.d >= 3;
        auto consider = [&](int x, int z, int ox, int oz) {
            if (!grid.inBounds(x, z) || !grid.inBounds(ox, oz)) return;
            if (at(x, z) != roles.edge) return;
            if (avoidCorners && (x == fp.x0 || x == fp.x0 + fp.w - 1) &&
                (z == fp.z0 || z == fp.z0 + fp.d - 1))
                return;
            const int outside = at(ox, oz);
            int score = -1;
            if (ts.walkable(outside)) {
                score = (outside == roles.path || outside == roles.plaza) ? 3 : 2;
            } else if (outside == roles.ground) {
                score = 1;
            }
            if (score > bestScore) {
                bestScore = score;
                bestX = x;
                bestZ = z;
            }
        };
        for (int x = fp.x0; x < fp.x0 + fp.w; ++x) {
            consider(x, fp.z0, x, fp.z0 - 1);
            consider(x, fp.z0 + fp.d - 1, x, fp.z0 + fp.d);
        }
        for (int z = fp.z0; z < fp.z0 + fp.d; ++z) {
            consider(fp.x0, z, fp.x0 - 1, z);
            consider(fp.x0 + fp.w - 1, z, fp.x0 + fp.w, z);
        }
        if (bestX < 0) {
            bestX = std::clamp(fp.x0 + fp.w / 2, fp.x0 + 1, fp.x0 + fp.w - 2);
            bestZ = fp.z0 + fp.d - 1;
        }
        fp.doorX = bestX;
        fp.doorZ = bestZ;
    }

    // --- spawn ----------------------------------------------------------------
    // The dreamer arrives at zone 0's anchor; that tile must be walkable no
    // matter what the solve did.
    if (sites.empty()) {
        out.notes.push_back("no zone sites — nothing to validate");
        return out;
    }
    const int sx = sites[0].x, sz = sites[0].y;
    if (!ts.walkable(at(sx, sz))) {
        grid.set(sx, sz, static_cast<TileId>(roles.plaza));
        ++out.repairs;
        out.notes.push_back("spawn tile forced walkable");
    }

    // --- reach + repair loop ---------------------------------------------------
    // Re-flood after each carve until everything required is connected.
    // Bounded by the number of targets, so it always terminates.
    auto targets = [&]() {
        std::vector<glm::ivec2> t;
        for (size_t i = 1; i < sites.size(); ++i) t.push_back(sites[i]);
        for (const FootprintPlan& fp : footprints)
            t.push_back({fp.doorX, fp.doorZ});
        return t;
    }();

    {
        FloodContext ctx(grid, ts, roles, footprints);
        std::vector<std::uint8_t> seen = floodFrom(ctx, sx, sz);
        for (const glm::ivec2& t : targets) {
            if (!grid.inBounds(t.x, t.y)) continue;
            if (!seen[static_cast<size_t>(t.y) * n + t.x]) ++out.unreachableZones;
        }
        for (const glm::ivec2& t : targets) {
            if (!grid.inBounds(t.x, t.y)) continue;
            if (seen[static_cast<size_t>(t.y) * n + t.x]) continue;
            int fx = sx, fz = sz;
            nearestReached(seen, n, t.x, t.y, fx, fz);
            out.repairs += carve(grid, ts, roles, fx, fz, t.x, t.y);
            // The carve may stop AT a door/ring cell; make sure the cell
            // itself joined the network, then re-flood for the next target.
            FloodContext ctx2(grid, ts, roles, footprints);
            seen = floodFrom(ctx2, sx, sz);
        }
    }

    // --- bridge end anchoring ---------------------------------------------
    // A deck run that stops a tile or two short of walkable land gets
    // extended to the shore; decks should connect things. Piers dangle over
    // the water by design and are left alone. Runs pre-corruption, so severed
    // stumps cut later survive as stumps.
    {
        int extended = 0;
        auto deckish = [&](int x, int z) {
            return grid.inBounds(x, z) &&
                   (at(x, z) == roles.deck || at(x, z) == roles.pier);
        };
        auto landish = [&](int x, int z) {
            if (!grid.inBounds(x, z)) return false;
            const int t = at(x, z);
            if (t == roles.deck || t == roles.pier) return false;
            return ts.walkable(t) || t == roles.ground;
        };
        for (int z = 0; z < n; ++z) {
            for (int x = 0; x < n; ++x) {
                if (at(x, z) != roles.deck) continue;
                // A run end: exactly one deck/pier 4-neighbor.
                int deg = 0, dx = 0, dz = 0;
                for (int k = 0; k < 4; ++k) {
                    if (deckish(x + kDX[k], z + kDZ[k])) {
                        ++deg;
                        dx = kDX[k];
                        dz = kDZ[k];
                    }
                }
                if (deg != 1) continue;
                bool anchored = false;
                for (int k = 0; k < 4; ++k)
                    if (landish(x + kDX[k], z + kDZ[k])) anchored = true;
                if (anchored) continue;
                // March straight away from the run; if shore is within two
                // water tiles, lay deck to it.
                const int ox = -dx, oz = -dz;
                int cx = x, cz = z, reach = -1;
                for (int step = 1; step <= 2; ++step) {
                    cx += ox;
                    cz += oz;
                    if (landish(cx, cz)) {
                        reach = step;
                        break;
                    }
                    if (!grid.inBounds(cx, cz) || at(cx, cz) != roles.water)
                        break;
                }
                for (int step = 1; step < reach; ++step) {
                    grid.set(x + ox * step, z + oz * step,
                             static_cast<TileId>(roles.deck));
                    ++extended;
                }
            }
        }
        if (extended > 0) {
            out.repairs += extended;
            out.notes.push_back("extended " + std::to_string(extended) +
                                " deck tiles to shore");
        }
    }

    // --- bridge hygiene -------------------------------------------------------
    // Decks the flood never reached are dream debris arcing between nothing;
    // drown them so "a bridge exists" always means "a crossing exists".
    {
        FloodContext ctx(grid, ts, roles, footprints);
        std::vector<std::uint8_t> seen = floodFrom(ctx, sx, sz);
        int drowned = 0;
        for (int z = 0; z < n; ++z) {
            for (int x = 0; x < n; ++x) {
                const size_t i = static_cast<size_t>(z) * n + x;
                if ((at(x, z) == roles.deck || at(x, z) == roles.pier) &&
                    !seen[i]) {
                    grid.set(x, z, static_cast<TileId>(roles.water));
                    ++drowned;
                }
            }
        }
        if (drowned > 0) {
            out.repairs += drowned;
            out.notes.push_back("drowned " + std::to_string(drowned) +
                                " orphan deck tiles");
        }
        out.reachable = std::move(seen);
    }

    if (out.unreachableZones > 0) {
        out.notes.push_back(std::to_string(out.unreachableZones) +
                            " targets needed carving");
    }
    return out;
}

} // namespace liminal::procgen
