// tileset.cpp — tile vocabulary: JSON overlay over the compiled-in fallback.

#include <liminal/procgen/tileset.hpp>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace liminal::procgen {

int TileSet::idOf(std::string_view name) const {
    for (int i = 0; i < count(); ++i) {
        if (m_tiles[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

int TileSet::roleId(std::string_view role) const {
    for (int i = 0; i < count(); ++i) {
        if (m_tiles[static_cast<size_t>(i)].role == role) return i;
    }
    return -1;
}

TileMask TileSet::roleMask(std::string_view role) const {
    TileMask m = 0;
    for (int i = 0; i < count(); ++i) {
        if (m_tiles[static_cast<size_t>(i)].role == role) m |= maskOf(i);
    }
    return m;
}

std::vector<float> TileSet::weights() const {
    std::vector<float> w;
    w.reserve(m_tiles.size());
    for (const TileDef& t : m_tiles) w.push_back(t.weight);
    return w;
}

void TileSet::symmetrize() {
    const int n = count();
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            if (m_tiles[static_cast<size_t>(a)].adj & maskOf(b)) {
                m_tiles[static_cast<size_t>(b)].adj |= maskOf(a);
            }
        }
    }
}

TileSet TileSet::fromJsonFile(const std::string& path, const TileSet& base,
                              std::string* err) {
    TileSet ts = base;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "missing " + path + " (builtin rules in effect)";
        return ts;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("tiles") ||
        !j["tiles"].is_object()) {
        if (err) *err = "unparseable " + path + " (builtin rules in effect)";
        return ts;
    }

    // The JSON replaces adjacency wholesale for any tile it mentions (so a
    // removed pair really is removed), and symmetrizes afterwards.
    std::vector<TileMask> jsonAllowed(static_cast<size_t>(ts.count()), 0);
    std::vector<bool> touched(static_cast<size_t>(ts.count()), false);
    for (auto it = j["tiles"].begin(); it != j["tiles"].end(); ++it) {
        const int ti = ts.idOf(it.key());
        if (ti < 0 || !it->is_object()) continue;
        TileDef& def = ts.m_tiles[static_cast<size_t>(ti)];
        touched[static_cast<size_t>(ti)] = true;
        if (it->contains("weight") && (*it)["weight"].is_number()) {
            def.weight = (*it)["weight"].get<float>();
        }
        if (it->contains("walkable") && (*it)["walkable"].is_boolean()) {
            def.walkable = (*it)["walkable"].get<bool>();
        }
        if (it->contains("gated") && (*it)["gated"].is_boolean()) {
            def.gated = (*it)["gated"].get<bool>();
        }
        if (it->contains("role") && (*it)["role"].is_string()) {
            def.role = (*it)["role"].get<std::string>();
        }
        if (it->contains("adj") && (*it)["adj"].is_array()) {
            for (const auto& a : (*it)["adj"]) {
                if (!a.is_string()) continue;
                const int at = ts.idOf(a.get<std::string>());
                if (at >= 0) jsonAllowed[static_cast<size_t>(ti)] |= ts.maskOf(at);
            }
        }
    }
    for (int i = 0; i < ts.count(); ++i) {
        if (touched[static_cast<size_t>(i)]) {
            ts.m_tiles[static_cast<size_t>(i)].adj = jsonAllowed[static_cast<size_t>(i)];
        }
    }
    ts.symmetrize();
    return ts;
}

TileSet defaultTileSet() {
    // Ids/order mirror the original Tile enum: grass, dirt_path, water,
    // shore, bridge_deck, building_floor, building_edge, pier, plaza, void.
    std::vector<TileDef> defs = {
        {"grass", 10.0f, 0, true, false, "ground"},
        {"dirt_path", 2.0f, 0, true, false, "path"},
        {"water", 1.0f, 0, false, false, "water"},
        {"shore", 1.0f, 0, true, false, "shore"},
        {"bridge_deck", 0.15f, 0, true, false, "deck"},
        {"building_floor", 0.4f, 0, false, true, "floor"},
        {"building_edge", 0.6f, 0, false, false, "edge"},
        {"pier", 0.2f, 0, true, false, "pier"},
        {"plaza", 0.7f, 0, true, false, "plaza"},
        {"void", 0.01f, 0, false, false, "void"},
    };
    enum { Grass, DirtPath, Water, Shore, BridgeDeck, BuildingFloor,
           BuildingEdge, Pier, Plaza, Void };
    auto allow = [&defs](int a, std::initializer_list<int> bs) {
        for (int b : bs) {
            defs[static_cast<size_t>(a)].adj |= static_cast<TileMask>(1u << b);
            defs[static_cast<size_t>(b)].adj |= static_cast<TileMask>(1u << a);
        }
    };
    allow(Grass, {Grass, DirtPath, Shore, Plaza, BuildingEdge, Void});
    // The void rim: anything the zone seeding can stamp (plaza hearts, path
    // waypoints) and buildings' wall rings may sit on a cliff edge — void
    // terrain pre-collapses huge swaths, so a stingy rim contradicts forever.
    allow(Void, {DirtPath, Plaza, BuildingEdge});
    allow(DirtPath, {DirtPath, Grass, Plaza, Shore, BuildingEdge});
    allow(Water, {Water, Shore, BridgeDeck, Pier});
    allow(Shore, {Shore, Water, Grass, DirtPath, Plaza, Pier, BridgeDeck,
                  BuildingEdge, Void});
    allow(BridgeDeck, {BridgeDeck, Water, Shore});
    allow(BuildingFloor, {BuildingFloor, BuildingEdge});
    allow(BuildingEdge, {BuildingEdge, BuildingFloor, Grass, DirtPath, Plaza,
                         Shore});
    allow(Pier, {Pier, Water, Shore});
    allow(Plaza, {Plaza, DirtPath, Grass, BuildingEdge, Shore});
    allow(Void, {Void});

    return TileSet(std::move(defs));
}

} // namespace liminal::procgen
