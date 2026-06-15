// lm.procgen.* — the full procgen toolkit exposed to Lua. Kept in its own
// translation unit so the heavy procgen headers (terrain/wfc/shape grammar)
// don't slow lua_bindings.cpp down.
//
// Determinism contract (HARD): every entry point takes an explicit seed or
// Rng; all randomness flows through procgen/rng.hpp. No rand()/random_device/
// time anywhere. lm.procgen.town{seed=...} reproduces the 04_wfc_town example
// orchestration with the same per-stage salts (seed ^ 0x5EED for WFC, seed ^
// 0x6E0D for the building Rng), using the default tileset/family params, so a
// fixed seed yields a deterministic and structurally equivalent world.
//
// The procgen→renderable bridge: procgen's mesh type is liminal::MeshData
// (BuiltPiece.mesh, TerrainField::buildMesh) — the SAME type the render Mesh
// ctor and lm.assets.add_mesh (chunk 2) consume. No conversion needed: a piece
// mesh returned here can be handed straight to lm.assets.add_mesh. MeshData was
// already bound opaque (no constructor) in lua_bindings.cpp; we only read its
// vertex count here.
#include "lua_bindings.hpp"

#include <liminal/core/assets.hpp>
#include <liminal/procgen/layout_validator.hpp>
#include <liminal/procgen/rng.hpp>
#include <liminal/procgen/shape_grammar.hpp>
#include <liminal/procgen/terrain.hpp>
#include <liminal/procgen/tileset.hpp>
#include <liminal/procgen/types.hpp>
#include <liminal/procgen/wfc.hpp>
#include <liminal/render/mesh.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace liminal::luabind {

namespace pg = liminal::procgen;

namespace {

// --- small Lua-table readers (present-checked, defaults preserved) -----------

int optInt(const sol::table& t, const char* key, int def) {
    sol::object o = t[key];
    if (o.is<int>()) return o.as<int>();
    if (o.is<double>()) return int(o.as<double>());
    return def;
}
float optFloat(const sol::table& t, const char* key, float def) {
    sol::object o = t[key];
    if (o.is<double>()) return float(o.as<double>());
    return def;
}
std::uint32_t optU32(const sol::table& t, const char* key, std::uint32_t def) {
    sol::object o = t[key];
    if (o.is<double>()) return std::uint32_t(std::int64_t(o.as<double>()));
    if (o.is<int>()) return std::uint32_t(o.as<int>());
    return def;
}
std::string optStr(const sol::table& t, const char* key, const std::string& def) {
    sol::object o = t[key];
    if (o.is<std::string>()) return o.as<std::string>();
    return def;
}
// Integer-keyed variant for positional pairs ({x, z} stored at Lua keys 1, 2).
// The named optInt(.,"1",.) string key never matches a Lua integer key.
int optIntAt(const sol::table& t, int key, int def) {
    sol::object o = t[key];
    if (o.is<int>()) return o.as<int>();
    if (o.is<double>()) return int(o.as<double>());
    return def;
}

// terrain kind / water enums from string names (defaults preserved).
pg::TerrainParams::Kind kindOf(const std::string& s,
                               pg::TerrainParams::Kind def) {
    using K = pg::TerrainParams::Kind;
    if (s == "plane") return K::Plane;
    if (s == "hills") return K::Hills;
    if (s == "canyon") return K::Canyon;
    if (s == "islands") return K::Islands;
    if (s == "flooded") return K::Flooded;
    if (s == "void") return K::Void;
    return def;
}
pg::TerrainParams::Water waterOf(const std::string& s,
                                 pg::TerrainParams::Water def) {
    using W = pg::TerrainParams::Water;
    if (s == "none") return W::None;
    if (s == "some") return W::Some;
    if (s == "lots") return W::Lots;
    return def;
}

// Resolve the TileSet JSON overlay path through Assets::resolve and forward it
// to TileSet::fromJsonFile (which opens the path itself).
pg::TileSet loadTileSet(sol::optional<std::string> jsonPath) {
    const pg::TileSet base = pg::defaultTileSet();
    if (!jsonPath || jsonPath->empty()) return base;
    const std::string resolved = Assets::resolve(*jsonPath);
    std::string err;
    pg::TileSet ts = pg::TileSet::fromJsonFile(resolved, base, &err);
    if (!err.empty()) {
        std::printf("[lua] lm.procgen.tileset: %s\n", err.c_str());
        std::fflush(stdout);
    }
    return ts;
}

// Build a FootprintPlan from a Lua table mapping its fields.
pg::FootprintPlan planFromTable(const sol::table& t) {
    pg::FootprintPlan p;
    p.zone = optInt(t, "zone", p.zone);
    p.style = optInt(t, "style", p.style);
    p.x0 = optInt(t, "x0", p.x0);
    p.z0 = optInt(t, "z0", p.z0);
    p.w = optInt(t, "w", p.w);
    p.d = optInt(t, "d", p.d);
    p.doorX = optInt(t, "door_x", p.doorX);
    p.doorZ = optInt(t, "door_z", p.doorZ);
    return p;
}

// Build a FamilyParams from a Lua table (keys mirror applyFamilyParamsJson).
pg::FamilyParams familyFromTable(const sol::table& t) {
    pg::FamilyParams p;
    p.floorHeight = optFloat(t, "floor_height", p.floorHeight);
    p.wallThickness = optFloat(t, "wall_thickness", p.wallThickness);
    p.foundation = optFloat(t, "foundation", p.foundation);
    p.minFloors = optInt(t, "min_floors", p.minFloors);
    p.maxFloors = optInt(t, "max_floors", p.maxFloors);
    p.roof = optStr(t, "roof", p.roof);
    p.windowEvery = optFloat(t, "window_every", p.windowEvery);
    p.windowW = optFloat(t, "window_w", p.windowW);
    p.windowH = optFloat(t, "window_h", p.windowH);
    p.windowSill = optFloat(t, "window_sill", p.windowSill);
    p.doorW = optFloat(t, "door_w", p.doorW);
    p.doorH = optFloat(t, "door_h", p.doorH);
    p.insetPerFloor = optFloat(t, "inset_per_floor", p.insetPerFloor);
    p.ruinChaos = optFloat(t, "ruin_chaos", p.ruinChaos);
    sol::object col = t["colonnade"];
    if (col.is<bool>()) p.colonnade = col.as<bool>();
    sol::object st = t["stairs"];
    if (st.is<bool>()) p.stairs = st.as<bool>();
    p.lampChance = optFloat(t, "lamp_chance", p.lampChance);
    return p;
}

// Opaque wrapper for the upstream WFC domain vector (terrain mask / stamped
// footprints) so Lua can pass it between stages without copying its semantics
// into Lua space.
struct TileMaskVec {
    std::vector<pg::TileMask> data;
};

// The same footprint stamp the 04_wfc_town example uses: claim a w x d rect of
// edge-ring + floor cells, spiraling out from (cx, cz). Returns true on a fit
// and fills plan's rect.
bool stampFootprint(std::vector<pg::TileMask>& mask, const pg::TileSet& ts,
                    int n, int cx, int cz, int w, int d,
                    pg::FootprintPlan& plan) {
    const pg::TileMask floorM = ts.roleMask("floor");
    const pg::TileMask edgeM = ts.roleMask("edge");
    auto idx = [n](int x, int z) { return static_cast<size_t>(z) * n + x; };
    for (int r = 0; r < 10; ++r) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                const int x0 = cx + dx - w / 2, z0 = cz + dz - d / 2;
                if (x0 < 1 || z0 < 1 || x0 + w >= n - 1 || z0 + d >= n - 1)
                    continue;
                bool ok = true;
                for (int z = z0; z < z0 + d && ok; ++z)
                    for (int x = x0; x < x0 + w && ok; ++x)
                        if ((mask[idx(x, z)] & (floorM | edgeM)) !=
                            (floorM | edgeM))
                            ok = false;
                if (!ok) continue;
                for (int z = z0; z < z0 + d; ++z) {
                    for (int x = x0; x < x0 + w; ++x) {
                        const bool ring = (z == z0 || z == z0 + d - 1 ||
                                           x == x0 || x == x0 + w - 1);
                        mask[idx(x, z)] = ring ? edgeM : floorM;
                    }
                }
                plan.x0 = x0;
                plan.z0 = z0;
                plan.w = w;
                plan.d = d;
                return true;
            }
        }
    }
    return false;
}

// --- usertypes ---------------------------------------------------------------

void bindUsertypes(sol::state& lua) {
    // Rng — deterministic xorshift32, the only sanctioned randomness source.
    lua.new_usertype<pg::Rng>(
        "Rng", sol::constructors<pg::Rng(std::uint32_t)>(),
        "next", &pg::Rng::next,
        "next01", &pg::Rng::next01,
        "range", &pg::Rng::range);

    lua.new_usertype<pg::HeightField>(
        "HeightField", sol::no_constructor,
        "nodes", &pg::HeightField::nodes,
        "cell", &pg::HeightField::cell,
        "half", &pg::HeightField::half,
        "has_water", &pg::HeightField::hasWater,
        "water_level", &pg::HeightField::waterLevel,
        "tile_height",
        [](pg::HeightField& hf, int x, int z) { return hf.tileHeight(x, z); },
        "tile_underwater",
        [](pg::HeightField& hf, int x, int z) {
            return hf.tileUnderwater(x, z);
        });

    lua.new_usertype<pg::TileGrid>(
        "TileGrid", sol::no_constructor,
        "n", &pg::TileGrid::n,
        "tile_size", &pg::TileGrid::tileSize,
        "in_bounds",
        [](pg::TileGrid& g, int x, int z) { return g.inBounds(x, z); },
        "at", [](pg::TileGrid& g, int x, int z) -> int { return g.at(x, z); },
        "set",
        [](pg::TileGrid& g, int x, int z, int id) {
            g.set(x, z, static_cast<pg::TileId>(id));
        },
        // center(x,z) -> (worldX, worldZ)
        "center",
        [](pg::TileGrid& g, int x, int z) -> std::tuple<float, float> {
            const glm::vec2 c = g.center(x, z);
            return {c.x, c.y};
        });

    lua.new_usertype<pg::TileSet>(
        "TileSet", sol::no_constructor,
        "count", &pg::TileSet::count,
        "name",
        [](pg::TileSet& ts, int id) -> std::string { return ts.name(id); },
        "id_of",
        [](pg::TileSet& ts, const std::string& n) { return ts.idOf(n); },
        "role_id",
        [](pg::TileSet& ts, const std::string& r) { return ts.roleId(r); },
        "walkable", [](pg::TileSet& ts, int id) { return ts.walkable(id); });

    lua.new_usertype<pg::WfcResult>(
        "WfcResult", sol::no_constructor,
        "solved", &pg::WfcResult::solved,
        "restarts", &pg::WfcResult::restarts);

    lua.new_usertype<pg::FootprintPlan>(
        "FootprintPlan",
        sol::factories([](sol::table t) { return planFromTable(t); }),
        "zone", &pg::FootprintPlan::zone,
        "style", &pg::FootprintPlan::style,
        "x0", &pg::FootprintPlan::x0,
        "z0", &pg::FootprintPlan::z0,
        "w", &pg::FootprintPlan::w,
        "d", &pg::FootprintPlan::d,
        "door_x", &pg::FootprintPlan::doorX,
        "door_z", &pg::FootprintPlan::doorZ);

    lua.new_usertype<pg::FamilyParams>(
        "FamilyParams",
        sol::factories([]() { return pg::FamilyParams{}; },
                       [](sol::table t) { return familyFromTable(t); }),
        "floor_height", &pg::FamilyParams::floorHeight,
        "wall_thickness", &pg::FamilyParams::wallThickness,
        "foundation", &pg::FamilyParams::foundation,
        "min_floors", &pg::FamilyParams::minFloors,
        "max_floors", &pg::FamilyParams::maxFloors,
        "roof", &pg::FamilyParams::roof,
        "ruin_chaos", &pg::FamilyParams::ruinChaos,
        "colonnade", &pg::FamilyParams::colonnade,
        "stairs", &pg::FamilyParams::stairs,
        "lamp_chance", &pg::FamilyParams::lampChance);

    lua.new_usertype<pg::DeckRun>(
        "DeckRun", sol::no_constructor,
        "chain_len", &pg::DeckRun::chainLen,
        "is_pier", &pg::DeckRun::isPier);

    // BuiltPiece — mesh is the renderable MeshData (hand to lm.assets.add_mesh).
    lua.new_usertype<pg::BuiltPiece>(
        "BuiltPiece", sol::no_constructor,
        "mesh", &pg::BuiltPiece::mesh,
        "zone", &pg::BuiltPiece::zone,
        "anchor", &pg::BuiltPiece::anchor,
        "vertex_count",
        [](pg::BuiltPiece& p) {
            return lua_Integer(p.mesh.vertices.size() / 8);
        },
        "door_count",
        [](pg::BuiltPiece& p) { return lua_Integer(p.doors.size()); },
        "lamp_count",
        [](pg::BuiltPiece& p) { return lua_Integer(p.lamps.size()); },
        // lamps()/doors_hinges() as arrays of vec3 for scripts that place them.
        "lamps",
        [](pg::BuiltPiece& p, sol::this_state s) -> sol::table {
            sol::state_view sv(s);
            sol::table out = sv.create_table();
            for (size_t i = 0; i < p.lamps.size(); ++i)
                out[i + 1] = p.lamps[i];
            return out;
        });

    lua.new_usertype<TileMaskVec>("TileMaskVec", sol::no_constructor);
}

// --- functions ---------------------------------------------------------------

void bindFunctions(sol::table& procgen, sol::state& /*lua*/) {
    procgen["rng"] = [](std::uint32_t seed) { return pg::Rng(seed); };

    procgen["tileset"] = [](sol::optional<std::string> jsonPath) {
        return loadTileSet(jsonPath);
    };

    procgen["terrain"] = [](sol::table opts) {
        pg::TerrainParams tp;
        tp.seed = optU32(opts, "seed", tp.seed);
        tp.kind = kindOf(optStr(opts, "kind", "plane"), tp.kind);
        tp.water = waterOf(optStr(opts, "water", "none"), tp.water);
        tp.gridN = optInt(opts, "n", tp.gridN);
        tp.tileSize = optFloat(opts, "tile_size", tp.tileSize);
        return pg::generateTerrain(tp);
    };

    // terrain_mask(hf, params_table, ts) -> opaque masks. params_table carries
    // the same fields terrain() consumed (kind/water/n/tile_size/seed) so the
    // mask logic matches the heightfield.
    procgen["terrain_mask"] = [](const pg::HeightField& hf, sol::table opts,
                                 const pg::TileSet& ts) {
        pg::TerrainParams tp;
        tp.seed = optU32(opts, "seed", tp.seed);
        tp.kind = kindOf(optStr(opts, "kind", "plane"), tp.kind);
        tp.water = waterOf(optStr(opts, "water", "none"), tp.water);
        tp.gridN = optInt(opts, "n", hf.nodes > 0 ? hf.nodes - 1 : tp.gridN);
        tp.tileSize = optFloat(opts, "tile_size", tp.tileSize);
        TileMaskVec v;
        v.data = pg::terrainMask(hf, tp, ts);
        return v;
    };

    procgen["stamp_footprint"] = [](TileMaskVec& masks, const pg::TileSet& ts,
                                    int n, int cx, int cz, int w, int d,
                                    sol::this_state s) -> sol::object {
        pg::FootprintPlan plan;
        if (stampFootprint(masks.data, ts, n, cx, cz, w, d, plan))
            return sol::make_object(s, plan);
        return sol::make_object(s, sol::lua_nil);
    };

    procgen["solve_wfc"] = [](sol::table opts) -> pg::WfcResult {
        sol::object tsO = opts["tileset"];
        sol::object mO = opts["masks"];
        if (!tsO.is<pg::TileSet>() || !mO.is<TileMaskVec>())
            return pg::WfcResult{};
        const pg::TileSet& ts = tsO.as<pg::TileSet&>();
        const TileMaskVec& masks = mO.as<TileMaskVec&>();
        const int n = optInt(opts, "n", 0);
        const std::uint32_t seed = optU32(opts, "seed", 1u);
        const int maxRestarts = optInt(opts, "max_restarts", 24);
        return pg::solveWfc(ts, ts.weights(), masks.data, n, seed, maxRestarts);
    };

    procgen["grid_from"] = [](const pg::WfcResult& res, int n,
                              float tileSize) {
        pg::TileGrid g;
        g.reset(n, tileSize);
        g.tiles = res.tiles;
        return g;
    };

    // validate(grid, hf, sites_table, plans_table, ts) -> {repairs=,
    // unreachable_zones=}. sites is an array of {x,z} pairs; plans an array of
    // FootprintPlan. Mutates grid and (returns) the plans with door fields set.
    procgen["validate"] = [](pg::TileGrid& grid, const pg::HeightField& hf,
                             sol::table sitesT, sol::table plansT,
                             const pg::TileSet& ts,
                             sol::this_state s) -> sol::table {
        std::vector<glm::ivec2> sites;
        for (auto& kv : sitesT) {
            sol::table pair = kv.second.as<sol::table>();
            sites.push_back({optInt(pair, "x", optIntAt(pair, 1, 0)),
                             optInt(pair, "z", optIntAt(pair, 2, 0))});
        }
        std::vector<pg::FootprintPlan> plans;
        for (auto& kv : plansT)
            plans.push_back(kv.second.as<pg::FootprintPlan>());
        pg::ValidateOutcome out =
            pg::validateAndRepair(grid, hf, sites, plans, ts);
        sol::state_view sv(s);
        sol::table r = sv.create_table();
        r["repairs"] = out.repairs;
        r["unreachable_zones"] = out.unreachableZones;
        // hand back the door-filled plans
        sol::table outPlans = sv.create_table();
        for (size_t i = 0; i < plans.size(); ++i) outPlans[i + 1] = plans[i];
        r["plans"] = outPlans;
        return r;
    };

    procgen["build_building"] = [](const pg::TileGrid& g,
                                   const pg::HeightField& hf,
                                   const pg::FootprintPlan& fp,
                                   const pg::FamilyParams& fam, pg::Rng& rng) {
        return pg::expandBuilding(g, hf, fp, fam, rng);
    };

    procgen["collect_runs"] = [](const pg::TileGrid& g, const pg::TileSet& ts,
                                 sol::this_state s) -> sol::table {
        sol::state_view sv(s);
        sol::table out = sv.create_table();
        const std::vector<pg::DeckRun> runs = pg::collectRuns(g, ts);
        for (size_t i = 0; i < runs.size(); ++i) out[i + 1] = runs[i];
        return out;
    };

    procgen["build_deck"] = [](const pg::TileGrid& g, const pg::HeightField& hf,
                               const pg::TileSet& ts, const pg::DeckRun& run) {
        return pg::expandDeck(g, hf, ts, run);
    };

    // terrain_mesh(hf) / water_mesh(hf) -> MeshData (renderable; pass to
    // lm.assets.add_mesh). Built through TerrainField, kind inferred from
    // hasWater (Islands when wet, Hills when dry — only colors water logic).
    procgen["terrain_mesh"] = [](const pg::HeightField& hf) {
        const pg::TerrainField::Kind k =
            hf.hasWater ? pg::TerrainField::Kind::Islands
                        : pg::TerrainField::Kind::Hills;
        pg::TerrainField field(hf, k);
        return field.buildMesh();
    };
    procgen["water_mesh"] = [](const pg::HeightField& hf) {
        pg::TerrainField field(hf, pg::TerrainField::Kind::Islands);
        return field.buildWaterMesh();
    };
}

// --- the one-shot town(): mirrors 04_wfc_town/main.cpp generate() ------------

sol::table buildTown(sol::table opts, sol::state& lua) {
    const std::uint32_t seed = optU32(opts, "seed", 1337u);
    const int n = optInt(opts, "n", 32);
    const float tileSize = optFloat(opts, "tile_size", 2.5f);

    sol::optional<std::string> tsPath;
    if (opts["tileset"].valid() && opts["tileset"].is<std::string>())
        tsPath = opts["tileset"].get<std::string>();
    const pg::TileSet ts = loadTileSet(tsPath);

    // Terrain (defaults match the example: islands + some water).
    pg::TerrainParams tp;
    tp.seed = seed;
    tp.kind = kindOf(optStr(opts, "terrain", "islands"),
                     pg::TerrainParams::Kind::Islands);
    tp.water =
        waterOf(optStr(opts, "water", "some"), pg::TerrainParams::Water::Some);
    tp.gridN = n;
    tp.tileSize = tileSize;
    const pg::HeightField terrain = pg::generateTerrain(tp);
    std::vector<pg::TileMask> mask = pg::terrainMask(terrain, tp, ts);

    // Two buildings near the middle, like the example.
    std::vector<pg::FootprintPlan> plans;
    pg::FootprintPlan a, b;
    a.zone = 0;
    b.zone = 0;
    if (stampFootprint(mask, ts, n, n / 2 - 5, n / 2, 5, 4, a))
        plans.push_back(a);
    if (stampFootprint(mask, ts, n, n / 2 + 5, n / 2 + 3, 6, 5, b))
        plans.push_back(b);

    // WFC solve (same salt as the example).
    const pg::WfcResult solved =
        pg::solveWfc(ts, ts.weights(), mask, n, seed ^ 0x5EEDu);
    pg::TileGrid grid;
    grid.reset(n, tileSize);
    grid.tiles = solved.tiles;

    // Validate / repair from the town heart.
    const std::vector<glm::ivec2> sites{{n / 2, n / 2}};
    const pg::ValidateOutcome outcome =
        pg::validateAndRepair(grid, terrain, sites, plans, ts);

    // Architecture. Optional `families` table overlays the default house; we
    // build one FamilyParams (the example uses a single house for every
    // footprint). Building Rng salt matches the example.
    pg::FamilyParams house;
    if (opts["families"].valid() && opts["families"].is<sol::table>()) {
        sol::table fams = opts["families"];
        // first entry, if any, configures the house family
        for (auto& kv : fams) {
            if (kv.second.is<sol::table>())
                house = familyFromTable(kv.second.as<sol::table>());
            break;
        }
    }
    pg::Rng rng(seed ^ 0x6E0Du);

    sol::table pieces = lua.create_table();
    int pi = 1;
    for (const pg::FootprintPlan& fp : plans)
        pieces[pi++] = pg::expandBuilding(grid, terrain, fp, house, rng);
    for (const pg::DeckRun& run : pg::collectRuns(grid, ts))
        pieces[pi++] = pg::expandDeck(grid, terrain, ts, run);

    sol::table result = lua.create_table();
    result["grid"] = grid;
    result["terrain"] = terrain;
    result["pieces"] = pieces;
    result["repairs"] = outcome.repairs;
    result["restarts"] = solved.restarts;
    return result;
}

} // namespace

void bindProcgen(sol::table& lm, sol::state& lua) {
    bindUsertypes(lua);

    sol::table procgen = lua.create_table();
    bindFunctions(procgen, lua);
    procgen["town"] = [&lua](sol::table opts) { return buildTown(opts, lua); };

    lm["procgen"] = procgen;
}

} // namespace liminal::luabind
