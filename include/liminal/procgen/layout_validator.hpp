#pragma once
// Stage 6: the guarantees. Flood-fill walkability from the spawn tile,
// verify every zone anchor and every footprint door is reachable and every
// bridge actually connects land, and REPAIR in place when they aren't —
// carving paths over land and bridge decks over water/void. Carving is
// unconditional, so validation never fails outright; the orchestrator
// re-rolls the WFC instead when the repair bill says the layout was junk.
//
// Tile semantics (what is walkable, what gates a building, what carving
// writes) come from the TileSet's flags and roles, never from hardcoded ids.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <liminal/procgen/tileset.hpp>
#include <liminal/procgen/types.hpp>

namespace liminal::procgen {

struct ValidateOutcome {
    int repairs = 0;            // tiles rewritten to restore reachability
    int unreachableZones = 0;   // before repair — the re-roll signal
    std::vector<std::uint8_t> reachable; // per tile, AFTER repair
    std::vector<std::string> notes;
};

// `sites` are the zone anchors in tile coordinates (site 0 = spawn).
// Footprint doors are chosen here (the solve decides what surrounds a
// building, so the door can only be picked after it). Mutates grid and
// the plans' door fields.
ValidateOutcome validateAndRepair(TileGrid& grid, const HeightField& terrain,
                                  const std::vector<glm::ivec2>& sites,
                                  std::vector<FootprintPlan>& footprints,
                                  const TileSet& ts);

} // namespace liminal::procgen
