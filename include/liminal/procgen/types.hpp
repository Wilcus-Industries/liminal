#pragma once
// Plain-struct contracts between procgen stages. Everything here is CPU-side
// on purpose: generation may run on a worker thread, and GL objects can only
// be created on the render thread — the app uploads MeshData when it commits
// a generated world.
//
// Tiles are opaque ids; their meaning (walkability, adjacency, roles) lives
// in the TileSet the app supplies. The toolkit never hardcodes a vocabulary.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <liminal/render/mesh.hpp>

namespace liminal::procgen {

// One macro-layout cell id. The TileSet defines what each id means.
using TileId = std::uint8_t;

// One bit per tile id: the WFC cell domain. Stages upstream of the solver
// (terrain, footprint stamping) constrain cells by shrinking their mask.
using TileMask = std::uint16_t;

struct TileGrid {
    int n = 48;             // tiles per side
    float tileSize = 2.5f;  // world units per tile
    std::vector<TileId> tiles; // n*n, row-major: index = z * n + x

    void reset(int sideN, float size, TileId fill = 0) {
        n = sideN;
        tileSize = size;
        tiles.assign(static_cast<size_t>(n) * n, fill);
    }
    bool inBounds(int x, int z) const { return x >= 0 && x < n && z >= 0 && z < n; }
    TileId at(int x, int z) const { return tiles[static_cast<size_t>(z) * n + x]; }
    void set(int x, int z, TileId t) { tiles[static_cast<size_t>(z) * n + x] = t; }

    float worldHalf() const { return n * tileSize * 0.5f; }
    // World-space center of tile (x, z); the grid is centered on the origin.
    glm::vec2 center(int x, int z) const {
        return {-worldHalf() + (x + 0.5f) * tileSize,
                -worldHalf() + (z + 0.5f) * tileSize};
    }
    // Tile containing a world position (clamped to the grid).
    void tileAt(float wx, float wz, int& outX, int& outZ) const {
        const float fx = (wx + worldHalf()) / tileSize;
        const float fz = (wz + worldHalf()) / tileSize;
        outX = std::max(0, std::min(n - 1, static_cast<int>(fx)));
        outZ = std::max(0, std::min(n - 1, static_cast<int>(fz)));
    }
};

// CPU heightfield aligned to the tile grid: (gridN + 1) nodes per side, one
// node on every tile corner, spacing == tileSize. Heights are coarsely
// quantized (chunky low-poly terrain). The water mask is what WFC consumes
// as pre-collapsed cells.
struct HeightField {
    int nodes = 0;     // gridN + 1
    float cell = 2.5f; // == tileSize
    float half = 60.0f;
    std::vector<float> heights; // nodes * nodes, row-major
    bool hasWater = false;
    float waterLevel = -1000.0f;

    int index(int ix, int iz) const { return iz * nodes + ix; }
    // Tile (x,z) is underwater if its center (mean of 4 corners) is below
    // the water level.
    float tileHeight(int x, int z) const {
        return 0.25f * (heights[index(x, z)] + heights[index(x + 1, z)] +
                        heights[index(x, z + 1)] + heights[index(x + 1, z + 1)]);
    }
    bool tileUnderwater(int x, int z) const {
        return hasWater && tileHeight(x, z) < waterLevel;
    }
};

// A building's claim on the grid, stamped into the WFC domains before the
// solve and expanded into geometry by the shape grammar after validation.
// The rect includes the wall ring (edge-role tiles); the door is one ring
// cell whose wall the grammar leaves open — it must touch a walkable tile
// outside. `style` is an app-defined family id (the app maps it to a
// FamilyParams rule set); the toolkit only carries it through.
struct FootprintPlan {
    int zone = 0;
    int style = 0;      // app-defined architecture family id
    int x0 = 0, z0 = 0; // min tile corner
    int w = 3, d = 3;   // extent in tiles (>= 3 so a floor cell exists)
    int doorX = -1, doorZ = -1;
};

// A hinged door panel for the app to animate. The panel's closed pose
// extends from the hinge along rotateY(yawClosed)'s +X by `width`; the app
// swings it about the hinge's Y axis and owns angle/collision/mesh.
struct DoorSpec {
    glm::vec3 hinge{0.0f}; // bottom of the hinge edge, on the floor
    float width = 1.3f;
    float height = 2.0f;
    float yawClosed = 0.0f; // radians; rotateY(yawClosed) maps +X to the wall dir
};

// An interior wall point for a decal/marking: world position at eye height
// plus the wall's inward-facing normal (the decal faces the room).
struct WallMark {
    glm::vec3 pos{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
};

// One shape-grammar product: world-frame vertices (draw with an identity
// model) plus world-space collision boxes, ready to upload. Apps that need
// extra dressing (name, theme, tint) derive from this.
struct BuiltPiece {
    MeshData mesh;
    std::vector<PartBox> boxes; // world space
    int zone = 0;
    glm::vec3 anchor{0.0f};   // representative world point (door / deck start)
    std::vector<DoorSpec> doors; // usually 0 or 1; ruins have none
    std::vector<glm::vec3> lamps;   // interior lamp positions (floor level)
    std::vector<WallMark> scrawls;  // interior wall mark anchors
};

} // namespace liminal::procgen
