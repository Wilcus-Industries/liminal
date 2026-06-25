#pragma once
// GPU mesh: interleaved vertex buffer (pos3 | normal3 | uv2), flat-shaded.
// All primitives are generated procedurally — no model files in Phase 1.
// Vertices are duplicated per face (no index buffer) so every face gets a
// hard flat normal, which is the PS1 low-poly look.

#include <vector>

#include <glm/glm.hpp>

namespace liminal {

// One primitive piece of a composite structure mesh. Engine-local vocabulary:
// the game layer translates its (LLM-authored) part specs into these, keeping
// this header free of game types. `at` is the piece's base-center, `size` its
// w/h/d extents — both in structure-local units; Mesh::structure() normalizes
// the assembled whole.
struct MeshPart {
    enum class Kind { Box, Wall, Pillar, Roof, Cone, Slab, Arch };
    Kind kind = Kind::Box;
    glm::vec3 at{0.0f};
    glm::vec3 size{1.0f};
};

// Local-space AABB of one collidable piece of a structure mesh, in the same
// (normalized) space as the mesh's vertices. The game collides against these
// instead of the whole-mesh AABB so the gaps in the LLM's floor plan stay
// walkable. An arch part contributes three (two jambs + lintel) so its
// opening is genuinely open.
struct PartBox {
    glm::vec3 mn{0.0f};
    glm::vec3 mx{0.0f};
    // The player may stand on this piece's top face (decks, steps, floors).
    // Non-walkable pieces still block horizontal movement but never offer
    // their top as ground, so you can't perch on a wall's edge mid-clip.
    bool walkable = false;
    // Non-solid pieces never block movement or count as touch — the player
    // walks straight through (a tree's canopy). Raycasts still hit them so
    // the piece remains targetable.
    bool solid = true;
};

struct MeshData {
    // Interleaved: x y z | nx ny nz | u v  (8 floats per vertex)
    std::vector<float> vertices;

    void addTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                     const glm::vec2& uvA, const glm::vec2& uvB, const glm::vec2& uvC);
    // Convenience: quad as two triangles (a,b,c,d counter-clockwise).
    void addQuad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
                 float uvScale = 1.0f);
};

class Mesh {
public:
    explicit Mesh(const MeshData& data);
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    void draw() const; // binds VAO, glDrawArrays

    // Local-space bounds, used by the game for collision AABBs and raycasts.
    glm::vec3 localMin{0.0f};
    glm::vec3 localMax{0.0f};

    // --- Primitive factories (unit-ish scale) -----------------------------
    // The architectural shapes (box, pyramid, pillar, arch, stair, plane, form)
    // are centered on their AABB so the local origin is the visual center (what
    // the MCP agent / DCC tools assume for Transform.position). The organic
    // props (blob, tree, rock, crystal) and structure() keep their base on y=0
    // for terrain placement.
    static Mesh box();                    // 1x1x1
    static Mesh pyramid();                // 1x1 base, height 1
    static Mesh pillar();                 // 0.4x0.4 footprint, height 2.4
    static Mesh arch();                   // two pillars + lintel, ~2 wide
    static Mesh blob(unsigned int seed);  // icosphere with noisy displacement
    static Mesh stair();                  // 5 ascending steps
    // Parametric prism the LLM authors: an n-gon column (sides 3..8) that can
    // twist up its height and taper toward the top (taper 0 = apex/cone).
    static Mesh form(int sides, float twist, float taper, unsigned int seed);
    // Natural props. Like blob, each is seeded so a recurring object keeps
    // its exact lumps.
    // `outBoxes`, when given, receives the trunk (solid) and canopy
    // (non-solid) collision boxes in mesh-local space, so the player can
    // stand under the crown instead of bouncing off the whole-tree AABB.
    static Mesh tree(unsigned int seed,      // trunk + canopy (round or pine)
                     std::vector<PartBox>* outBoxes = nullptr);
    static Mesh rock(unsigned int seed);     // squashed lumpy icosphere
    static Mesh crystal(unsigned int seed);  // cluster of tilted pointed prisms
    // Composite structure assembled from LLM-authored parts: all pieces merged
    // into one mesh, recentered on x/z, base sunk to y=0, footprint normalized
    // to <= ~4.5 units so the object's own scale stays in charge of size.
    // `outBoxes`, when given, receives one collision AABB per solid piece in
    // the same normalized local space as the vertices.
    static Mesh structure(const std::vector<MeshPart>& parts, unsigned int seed,
                          std::vector<PartBox>* outBoxes = nullptr);
    static Mesh groundPlane(float halfSize, float uvTiles);
    static Mesh quad();   // unit standing quad in the XY plane (z=0), for billboards/decals
    static Mesh plane();  // a thin flat walkable slab, 1x1 footprint, AABB-centered

    // Stair geometry, shared so the collision system can reconstruct each step's
    // top surface (the single mesh AABB hides them). Step i (0..kStairSteps-1)
    // spans local z in [i*kStairRun, (i+1)*kStairRun], local x in
    // [-kStairHalfWidth, kStairHalfWidth], with its top at (i+1)*kStairRise.
    static constexpr int   kStairSteps     = 5;
    static constexpr float kStairRise      = 0.3f;
    static constexpr float kStairRun       = 0.45f;
    static constexpr float kStairHalfWidth = 0.7f;

private:
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    int m_vertexCount = 0;
};

} // namespace liminal
