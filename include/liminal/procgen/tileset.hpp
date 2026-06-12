#pragma once
// The data-driven tile vocabulary. A TileSet owns everything the layout
// stages need to know about a tile id: its name, WFC base weight and
// adjacency mask, whether it is walkable, whether it is door-gated (walkable
// only through a chosen threshold cell), and an optional semantic role the
// generic generators key off ("path", "deck", "water", ...) instead of
// hardcoded enum values. Tiles load from JSON over a compiled-in fallback,
// so the vocabulary is an asset, not a code change.
//
// Roles the toolkit understands (a tile may carry at most one):
//   ground - open land filler (door scoring treats it as acceptable)
//   path   - carved walk line over land (repairs carve this)
//   water  - impassable liquid (repairs bridge it with `deck`)
//   shore  - land/water seam
//   deck   - elevated crossing over water/void
//   floor  - building interior (the gated tile)
//   edge   - building wall ring (door cells are chosen on it)
//   pier   - deck that may dangle over water unanchored
//   plaza  - paved gathering ground (preferred door frontage)
//   void   - nothing; falling territory (repairs bridge it with `deck`)

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <liminal/procgen/types.hpp>

namespace liminal::procgen {

struct TileDef {
    std::string name;
    float weight = 1.0f; // WFC base weight
    TileMask adj = 0;    // tiles allowed 4-adjacent (kept symmetric)
    bool walkable = false;
    bool gated = false;  // enterable only through a door cell (e.g. floor)
    std::string role;    // semantic role, "" = none
};

class TileSet {
public:
    TileSet() = default;
    explicit TileSet(std::vector<TileDef> defs) : m_tiles(std::move(defs)) {
        symmetrize();
    }

    int count() const { return static_cast<int>(m_tiles.size()); }
    const TileDef& def(int id) const { return m_tiles[static_cast<size_t>(id)]; }

    TileMask maskOf(int id) const { return static_cast<TileMask>(1u << id); }
    TileMask allMask() const {
        return static_cast<TileMask>((1u << count()) - 1u);
    }

    const std::string& name(int id) const { return def(id).name; }
    float weight(int id) const { return def(id).weight; }
    TileMask adjacent(int id) const { return def(id).adj; }
    bool walkable(int id) const { return def(id).walkable; }
    bool gated(int id) const { return def(id).gated; }

    // -1 when no tile carries the name/role.
    int idOf(std::string_view name) const;
    int roleId(std::string_view role) const;
    // OR of maskOf over every tile with the role (0 when none).
    TileMask roleMask(std::string_view role) const;

    // Base weights as a mutable working copy (spec semantics multiply these
    // before a solve).
    std::vector<float> weights() const;

    // A pair is allowed if EITHER side lists it; keeps data hand-editable
    // without double bookkeeping. Constructor calls this; call again after
    // mutating adj.
    void symmetrize();

    // JSON overlay (schema: {"tiles": {<name>: {"weight": f, "adj": [names],
    // "walkable": b, "gated": b, "role": s}}}) on top of `base`. A mentioned
    // tile's adjacency is replaced wholesale; unmentioned fields keep the
    // base values; unknown tile names are ignored. On any read/parse error
    // the base is returned untouched and *err says why.
    static TileSet fromJsonFile(const std::string& path, const TileSet& base,
                                std::string* err = nullptr);

private:
    std::vector<TileDef> m_tiles;
};

// The compiled-in fallback vocabulary (grass/path/water/shore/deck/floor/
// edge/pier/plaza/void) the consuming app's JSON overlays it.
TileSet defaultTileSet();

} // namespace liminal::procgen
