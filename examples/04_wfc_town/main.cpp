// 04_wfc_town: the whole procgen toolkit, engine-only. A TileSet (JSON over
// the compiled-in defaults) feeds noise terrain -> WFC layout -> validation
// -> shape-grammar buildings and deck spans, all deterministic in one seed,
// rendered through the retro Renderer under a slow orbit camera. ESC quits;
// SPACE reseeds and regenerates the town.
#include <liminal/core/window.hpp>
#include <liminal/procgen/layout_validator.hpp>
#include <liminal/procgen/rng.hpp>
#include <liminal/procgen/shape_grammar.hpp>
#include <liminal/procgen/terrain.hpp>
#include <liminal/procgen/tileset.hpp>
#include <liminal/procgen/types.hpp>
#include <liminal/procgen/wfc.hpp>
#include <liminal/render/mesh.hpp>
#include <liminal/render/renderer.hpp>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <memory>
#include <vector>

namespace pg = liminal::procgen;

namespace {

struct Town {
    pg::TileGrid grid;
    pg::HeightField terrain;
    std::unique_ptr<pg::TerrainField> field;
    std::unique_ptr<liminal::Mesh> ground;
    std::unique_ptr<liminal::Mesh> water;
    std::vector<std::unique_ptr<liminal::Mesh>> pieces;
    std::vector<glm::vec3> pieceColors;
};

// Claim a w x d footprint rect (edge ring + floor interior) on cells whose
// domain still allows building tiles; spiral out from (cx, cz).
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

Town generate(std::uint32_t seed) {
    Town town;

    // The vocabulary: example JSON over the compiled-in defaults.
    std::string err;
    const pg::TileSet ts = pg::TileSet::fromJsonFile(
        WFC_TOWN_ASSETS "/tiles.json", pg::defaultTileSet(), &err);
    if (!err.empty()) std::fprintf(stderr, "[town] %s\n", err.c_str());

    // Terrain: a small island field with some water.
    pg::TerrainParams tp;
    tp.seed = seed;
    tp.kind = pg::TerrainParams::Kind::Islands;
    tp.water = pg::TerrainParams::Water::Some;
    tp.gridN = 32;
    tp.tileSize = 2.5f;
    town.terrain = pg::generateTerrain(tp);
    std::vector<pg::TileMask> mask = pg::terrainMask(town.terrain, tp, ts);

    // Two buildings near the middle, then solve the rest with WFC.
    std::vector<pg::FootprintPlan> plans;
    pg::FootprintPlan a, b;
    a.zone = 0;
    b.zone = 0;
    if (stampFootprint(mask, ts, tp.gridN, tp.gridN / 2 - 5, tp.gridN / 2, 5,
                       4, a))
        plans.push_back(a);
    if (stampFootprint(mask, ts, tp.gridN, tp.gridN / 2 + 5, tp.gridN / 2 + 3,
                       6, 5, b))
        plans.push_back(b);

    const pg::WfcResult solved = pg::solveWfc(ts, ts.weights(), mask, tp.gridN,
                                              seed ^ 0x5EEDu);
    town.grid.reset(tp.gridN, tp.tileSize);
    town.grid.tiles = solved.tiles;

    // Guarantees: every door reachable from the town heart.
    const std::vector<glm::ivec2> sites{{tp.gridN / 2, tp.gridN / 2}};
    pg::validateAndRepair(town.grid, town.terrain, sites, plans, ts);

    // Ground + water meshes.
    town.field = std::make_unique<pg::TerrainField>(
        town.terrain, pg::TerrainField::Kind::Islands);
    town.ground = std::make_unique<liminal::Mesh>(town.field->buildMesh());
    if (town.field->hasWater()) {
        town.water =
            std::make_unique<liminal::Mesh>(town.field->buildWaterMesh());
    }

    // Architecture: the example house family for every footprint, plus the
    // deck spans the solve/repair laid over the water.
    pg::FamilyParams house;
    pg::applyFamilyParamsJson(house, WFC_TOWN_ASSETS "/house.json");
    pg::Rng rng(seed ^ 0x6E0Du);
    const glm::vec3 houseColors[2] = {{0.78f, 0.62f, 0.46f},
                                      {0.62f, 0.66f, 0.74f}};
    int built = 0;
    for (const pg::FootprintPlan& fp : plans) {
        const pg::BuiltPiece piece =
            pg::expandBuilding(town.grid, town.terrain, fp, house, rng);
        town.pieces.push_back(std::make_unique<liminal::Mesh>(piece.mesh));
        town.pieceColors.push_back(houseColors[built++ % 2]);
    }
    for (const pg::DeckRun& run : pg::collectRuns(town.grid, ts)) {
        const pg::BuiltPiece piece =
            pg::expandDeck(town.grid, town.terrain, ts, run);
        town.pieces.push_back(std::make_unique<liminal::Mesh>(piece.mesh));
        town.pieceColors.push_back({0.55f, 0.45f, 0.34f});
    }
    std::fprintf(stderr,
                 "[town] seed %u: %zu buildings, %zu pieces total\n", seed,
                 plans.size(), town.pieces.size());
    return town;
}

} // namespace

int main() {
    liminal::Window window(1280, 720, "liminal — 04_wfc_town");
    liminal::Renderer renderer;
    renderer.settings.skyColor = {0.55f, 0.62f, 0.70f};
    renderer.settings.fogColor = {0.55f, 0.62f, 0.70f};
    renderer.settings.fogDensity = 0.016f;

    std::uint32_t seed = 1337u;
    Town town = generate(seed);

    bool spaceWasDown = false;
    while (!window.shouldClose()) {
        window.pollEvents();
        if (window.keyPressed(GLFW_KEY_ESCAPE)) window.requestClose();
        const bool spaceDown = window.keyPressed(GLFW_KEY_SPACE);
        if (spaceDown && !spaceWasDown) {
            seed = seed * 1664525u + 1013904223u;
            town = generate(seed);
        }
        spaceWasDown = spaceDown;

        // Slow orbit over the town.
        const float t = float(window.time()) * 0.15f;
        const float r = 34.0f;
        const glm::vec3 eye{std::cos(t) * r, 22.0f, std::sin(t) * r};
        const glm::mat4 view =
            glm::lookAt(eye, glm::vec3(0.0f, 0.0f, 0.0f),
                        glm::vec3(0.0f, 1.0f, 0.0f));
        renderer.beginFrame(view);

        liminal::DrawItem item;
        item.mesh = town.ground.get();
        item.color = {0.45f, 0.58f, 0.38f};
        item.color2 = {0.55f, 0.66f, 0.45f};
        renderer.draw(item);
        if (town.water) {
            item.mesh = town.water.get();
            item.color = {0.30f, 0.42f, 0.55f};
            item.color2 = item.color;
            renderer.draw(item);
        }
        for (size_t i = 0; i < town.pieces.size(); ++i) {
            item.mesh = town.pieces[i].get();
            item.color = town.pieceColors[i];
            item.color2 = town.pieceColors[i] * 1.15f;
            renderer.draw(item);
        }

        int fbw = 0, fbh = 0;
        window.framebufferSize(fbw, fbh);
        renderer.endFrame(fbw, fbh);
        window.swapBuffers();
    }
    return 0;
}
