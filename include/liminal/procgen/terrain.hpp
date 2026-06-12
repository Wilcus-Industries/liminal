#pragma once
// Noise terrain. TerrainParams -> a chunky quantized heightfield sized
// exactly to the WFC tile grid, plus the water/void mask the solver consumes
// as pre-collapsed cell domains. Deterministic in params.seed; no other
// input exists. Walkability is NOT guaranteed here — the layout validator
// owns that; this stage only promises determinism and a sane water table.
//
// TerrainField wraps a heightfield for runtime queries (bilinear height,
// water distance) and builds the faceted ground/water meshes.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <liminal/procgen/tileset.hpp>
#include <liminal/procgen/types.hpp>
#include <liminal/render/mesh.hpp>

namespace liminal::procgen {

struct TerrainParams {
    enum class Kind { Plane, Hills, Canyon, Islands, Flooded, Void };
    enum class Water { None, Some, Lots };

    std::uint32_t seed = 1u;
    Kind kind = Kind::Plane;
    Water water = Water::None;
    int gridN = 48;
    float tileSize = 2.5f;
};

HeightField generateTerrain(const TerrainParams& p);

// Per-tile WFC domain restriction from the terrain alone, expressed through
// the TileSet's roles:
//   underwater          -> water | deck | pier        (decks may span it)
//   land beside water   -> shore | pier               (the coastline)
//   below the void rim  -> void                       (nothing, ever)
//   anything else       -> everything minus the wet/void/coast vocabulary
std::vector<TileMask> terrainMask(const HeightField& hf, const TerrainParams& p,
                                  const TileSet& ts);

// Deterministic heightfield wrapper for a generated area. The legacy
// constructor turns (kind, seed) into a grid of heights, flattened under
// every zone disk and along every connection path so the requested layout is
// guaranteed walkable. The pipeline constructor wraps an already-generated
// HeightField for runtime queries and mesh building.
class TerrainField {
public:
    using Kind = TerrainParams::Kind;

    static constexpr float kCell = 3.0f;        // legacy grid spacing, world units
    static constexpr float kVoidFloor = -28.0f; // "ground" of the void kind

    // `connections` are zone-index pairs; a path ribbon is flattened along
    // each. Out-of-range or self edges are ignored.
    TerrainField(Kind kind, int seed, float halfSize,
                 const std::vector<glm::vec2>& zoneCenters,
                 const std::vector<glm::ivec2>& connections);

    // Pipeline path: wrap an already-generated heightfield for runtime
    // queries and mesh building. `kind` only colors hasWater-style logic.
    TerrainField(const HeightField& hf, Kind kind);

    // Bilinear height at a world position (clamped to the field's extent).
    float height(float x, float z) const;

    Kind kind() const { return m_kind; }
    float halfSize() const { return m_half; }
    bool hasWater() const { return m_hasWater; }
    float waterLevel() const { return m_waterLevel; }

    // Approximate world-unit distance to the nearest water surface, from a
    // BFS over the grid. Large (>= 9000) when the area has no water at all.
    float waterDistance(float x, float z) const;

    // Faceted terrain mesh (two hard-shaded triangles per cell, world-space
    // UVs so the ground texture tiles evenly regardless of slope).
    MeshData buildMesh(float uvPerUnit = 0.25f) const;
    // Same, but only the cells whose mask byte is non-zero ((n-1)^2 cells,
    // row-major), so the ground can be drawn in per-tile-type layers.
    MeshData buildMeshMasked(const std::vector<std::uint8_t>& cells,
                             float uvPerUnit = 0.25f) const;
    // One big quad at the water level (empty mesh when hasWater() is false).
    MeshData buildWaterMesh(float uvTiles = 48.0f) const;

private:
    int index(int ix, int iz) const { return iz * m_n + ix; }
    glm::vec3 nodePos(int ix, int iz) const;
    void computeWaterDistance();

    Kind m_kind = Kind::Plane;
    float m_half = 150.0f;
    float m_cell = kCell; // node spacing (the pipeline grid uses its own)
    int m_n = 0; // nodes per side
    std::vector<float> m_heights;
    std::vector<float> m_waterDist; // per node, world units
    bool m_hasWater = false;
    float m_waterLevel = 0.0f;
};

} // namespace liminal::procgen
