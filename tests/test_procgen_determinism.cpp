// Golden determinism test for the procgen pipeline. Pure CPU — no window, no
// GL: terrain -> mask -> footprint stamp -> WFC solve -> validate/repair ->
// shape-grammar expansion, all hashed with FNV-1a 64 and compared against a
// recorded golden. If this fails, a change broke the determinism contract
// (same seed => same world) or intentionally changed generation; re-record
// by running with LIMINAL_RECORD_GOLDEN=1 and updating kGolden below.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <liminal/procgen/layout_validator.hpp>
#include <liminal/procgen/rng.hpp>
#include <liminal/procgen/shape_grammar.hpp>
#include <liminal/procgen/terrain.hpp>
#include <liminal/procgen/tileset.hpp>
#include <liminal/procgen/types.hpp>
#include <liminal/procgen/wfc.hpp>

namespace pg = liminal::procgen;

namespace {

// Recorded golden (this pipeline, this seed). Re-record on intentional
// generation changes: LIMINAL_RECORD_GOLDEN=1 ./test_procgen_determinism
constexpr std::uint64_t kGolden = 0x456cecfe6d7a606cull;

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void u32(std::uint32_t v) { bytes(&v, sizeof v); }
    void f32(float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        u32(bits);
    }
    void floats(const std::vector<float>& v) {
        for (float f : v) f32(f);
    }
};

// Same stamping the 04_wfc_town example does: claim a w x d rect of
// edge-ring + floor cells wherever the mask permits, scanning rows.
bool stampFootprint(std::vector<pg::TileMask>& mask, const pg::TileSet& ts,
                    int n, int startX, int startZ, int w, int d,
                    pg::FootprintPlan& plan) {
    const pg::TileMask edgeM = ts.roleMask("edge");
    const pg::TileMask floorM = ts.roleMask("floor");
    const pg::TileMask waterM = ts.roleMask("water") | ts.roleMask("void");
    auto idx = [n](int x, int z) { return static_cast<size_t>(z) * n + x; };
    for (int z0 = startZ; z0 + d < n - 1; ++z0) {
        for (int x0 = startX; x0 + w < n - 1; ++x0) {
            bool ok = true;
            for (int z = z0; ok && z < z0 + d; ++z)
                for (int x = x0; ok && x < x0 + w; ++x)
                    if ((mask[idx(x, z)] & waterM) == mask[idx(x, z)]) ok = false;
            if (!ok) continue;
            for (int z = z0; z < z0 + d; ++z)
                for (int x = x0; x < x0 + w; ++x) {
                    const bool ring = (z == z0 || z == z0 + d - 1 ||
                                       x == x0 || x == x0 + w - 1);
                    mask[idx(x, z)] = ring ? edgeM : floorM;
                }
            plan.x0 = x0;
            plan.z0 = z0;
            plan.w = w;
            plan.d = d;
            return true;
        }
    }
    return false;
}

void hashPiece(Fnv& f, const pg::BuiltPiece& p) {
    f.floats(p.mesh.vertices);
    for (const auto& b : p.boxes) {
        f.f32(b.mn.x); f.f32(b.mn.y); f.f32(b.mn.z);
        f.f32(b.mx.x); f.f32(b.mx.y); f.f32(b.mx.z);
        f.u32(b.walkable ? 1u : 0u);
        f.u32(b.solid ? 1u : 0u);
    }
    for (const auto& d : p.doors) {
        f.f32(d.hinge.x); f.f32(d.hinge.y); f.f32(d.hinge.z);
        f.f32(d.width); f.f32(d.height); f.f32(d.yawClosed);
    }
}

} // namespace

int main() {
    constexpr std::uint32_t seed = 20260612u;

    const pg::TileSet ts = pg::defaultTileSet();

    pg::TerrainParams tp;
    tp.seed = seed;
    tp.kind = pg::TerrainParams::Kind::Hills;
    tp.water = pg::TerrainParams::Water::None;
    tp.gridN = 24;
    tp.tileSize = 2.5f;
    const pg::HeightField terrain = pg::generateTerrain(tp);
    std::vector<pg::TileMask> mask = pg::terrainMask(terrain, tp, ts);

    std::vector<pg::FootprintPlan> plans;
    pg::FootprintPlan a, b;
    if (stampFootprint(mask, ts, tp.gridN, 4, 6, 5, 4, a)) plans.push_back(a);
    if (stampFootprint(mask, ts, tp.gridN, 14, 14, 6, 5, b)) plans.push_back(b);
    if (plans.empty()) {
        std::fprintf(stderr, "FAIL: no footprint fit the terrain\n");
        return 1;
    }

    const pg::WfcResult solved =
        pg::solveWfc(ts, ts.weights(), mask, tp.gridN, seed ^ 0x5EEDu);

    if (!solved.solved) {
        std::fprintf(stderr, "FAIL: WFC did not solve\n");
        return 1;
    }

    pg::TileGrid grid;
    grid.reset(tp.gridN, tp.tileSize);
    grid.tiles = solved.tiles;

    const std::vector<glm::ivec2> sites{{tp.gridN / 2, tp.gridN / 2}};
    pg::validateAndRepair(grid, terrain, sites, plans, ts);

    Fnv f;
    f.u32(solved.solved ? 1u : 0u);
    f.u32(static_cast<std::uint32_t>(solved.restarts));
    f.bytes(grid.tiles.data(), grid.tiles.size() * sizeof(pg::TileId));
    f.floats(terrain.heights);

    const pg::FamilyParams house; // defaults are a full rule set
    pg::Rng rng(seed ^ 0x6E0Du);
    for (const pg::FootprintPlan& fp : plans) {
        f.u32(static_cast<std::uint32_t>(fp.doorX));
        f.u32(static_cast<std::uint32_t>(fp.doorZ));
        hashPiece(f, pg::expandBuilding(grid, terrain, fp, house, rng));
    }
    for (const pg::DeckRun& run : pg::collectRuns(grid, ts)) {
        hashPiece(f, pg::expandDeck(grid, terrain, ts, run));
    }

    std::printf("procgen pipeline hash: %016llx\n",
                static_cast<unsigned long long>(f.h));

    if (std::getenv("LIMINAL_RECORD_GOLDEN")) {
        std::printf("record this as kGolden in %s\n", __FILE__);
        return 0;
    }
    if (f.h != kGolden) {
        std::fprintf(stderr,
                     "FAIL: hash %016llx != golden %016llx — determinism "
                     "contract broken (or re-record after an intentional "
                     "generation change)\n",
                     static_cast<unsigned long long>(f.h),
                     static_cast<unsigned long long>(kGolden));
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
