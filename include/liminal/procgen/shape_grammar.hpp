#pragma once
// Architecture. Expands validated footprints (and the grid's deck/pier runs)
// into low-poly world-frame geometry through rewrite rules:
// building -> foundation, floors(1..N), roof ; floor -> wall runs with
// window/door slots ; deck run -> slabs + posts. The rules are data — a
// FamilyParams per architecture family, optionally overlaid from JSON — so
// new architecture is a data edit, not a code change.
//
// Guarantee owed downstream: every doored building's doorway opens onto the
// footprint's door tile, which the layout validator has already connected to
// the walkable network.

#include <string>
#include <vector>

#include <liminal/procgen/rng.hpp>
#include <liminal/procgen/tileset.hpp>
#include <liminal/procgen/types.hpp>

namespace liminal::procgen {

// Per-family rule parameters, the data side of the grammar.
struct FamilyParams {
    float floorHeight = 2.4f;
    float wallThickness = 0.32f;
    float foundation = 0.3f;     // slab the building stands on
    int minFloors = 1, maxFloors = 2;
    std::string roof = "gable";  // flat | gable | pyramid | none
    float windowEvery = 2.6f;    // run-length per window slot
    float windowW = 0.9f, windowH = 1.0f, windowSill = 1.05f;
    float doorW = 1.3f, doorH = 2.0f;
    float insetPerFloor = 0.0f;  // towers shrink as they stack
    float ruinChaos = 0.0f;      // 0..1: crumbled segment odds / height loss
    bool colonnade = false;      // pillar ring instead of upper walls
    bool stairs = true;          // interior stair runs between floors (+ flat roof)
    float lampChance = 0.5f;     // per-floor odds of one interior lamp
};

// Overlays the JSON at `path` (keys: floor_height, wall_thickness,
// foundation, min_floors, max_floors, roof, window_every, window_w,
// window_h, window_sill, door_w, door_h, inset_per_floor, ruin_chaos,
// colonnade, stairs, lamp_chance) onto `p`. A missing file leaves `p`
// untouched (the defaults are a full rule set); an unparseable one is
// reported to stderr and ignored.
void applyFamilyParamsJson(FamilyParams& p, const std::string& path);

// One building, world frame. The door side/offset comes from the plan's
// door tile (chosen by the validator). Draws on `rng` for floor count,
// ruin crumble, lamps and wall marks — call order is part of the
// determinism contract.
BuiltPiece expandBuilding(const TileGrid& g, const HeightField& terrain,
                          const FootprintPlan& fp, const FamilyParams& p,
                          Rng& rng);

// A connected deck/pier component of the grid, ordered into a chain (plus
// stray branch cells past chainLen).
struct DeckRun {
    std::vector<int> cells; // ordered chain of tile indices (+ branch cells)
    int chainLen = 0;       // cells[0..chainLen) is the chain; rest are branches
    bool isPier = false;
};

// Every deck/pier component in the grid (roles "deck" / "pier").
std::vector<DeckRun> collectRuns(const TileGrid& g, const TileSet& ts);

// Slab deck with posts down to the terrain; slab tops interpolate between
// the land heights at the two chain ends so every per-tile rise stays a
// step. Unanchored ends become bare stumps.
BuiltPiece expandDeck(const TileGrid& g, const HeightField& hf,
                      const TileSet& ts, const DeckRun& run);

} // namespace liminal::procgen
