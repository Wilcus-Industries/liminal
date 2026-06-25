// Mesh.cpp — interleaved flat-shaded GPU meshes + procedural primitives.
//
// Design notes:
//   - No index buffer, ever. Flat shading needs a distinct normal per *face*,
//     and a shared vertex can only carry one normal, so indexing would force
//     smooth normals (or normal-splitting bookkeeping). Duplicating the three
//     vertices of every triangle is wasteful on paper but these meshes are
//     tens of triangles — the PS1 itself pushed geometry the same way.
//   - Origin convention is split. The architectural primitives (box, pyramid,
//     pillar, arch, stair, plane, form) are centered on their AABB so the
//     object's local origin is its visual center — what DCC tools and the MCP
//     agent assume when they read/write Transform.position. The organic props
//     (blob, tree, rock, crystal) and the composite structure() keep their base
//     on y=0, centered on x/z: procgen places those by ground position + scale,
//     so baking the origin at the foot keeps that placement math trivial. quad
//     is XY-centered (billboard pivot); groundPlane is flat at y=0.
//   - Winding is kept consistently CCW-from-outside anyway, even though the
//     renderer never enables face culling: the normals come from the winding
//     (cross product), and lighting cares even if culling doesn't.

#include <liminal/render/mesh.hpp>

#include "hash_detail.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace liminal {

// --- MeshData ---------------------------------------------------------------

void MeshData::addTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                           const glm::vec2& uvA, const glm::vec2& uvB, const glm::vec2& uvC) {
    // Flat face normal from the winding. Degenerate triangles (collinear or
    // coincident points) produce a near-zero cross product; normalizing that
    // would spray NaNs through the vertex buffer, so guard and fall back to
    // "up" — a degenerate face is invisible anyway, it just must not poison
    // the lighting math.
    glm::vec3 n = glm::cross(b - a, c - a);
    float len = glm::length(n);
    n = (len > 1e-8f) ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);

    const glm::vec3 pos[3] = {a, b, c};
    const glm::vec2 uv[3] = {uvA, uvB, uvC};
    for (int i = 0; i < 3; ++i) {
        vertices.push_back(pos[i].x);
        vertices.push_back(pos[i].y);
        vertices.push_back(pos[i].z);
        vertices.push_back(n.x);
        vertices.push_back(n.y);
        vertices.push_back(n.z);
        vertices.push_back(uv[i].x);
        vertices.push_back(uv[i].y);
    }
}

void MeshData::addQuad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                       const glm::vec3& d, float uvScale) {
    // Two triangles sharing the a-c diagonal. uvScale > 1 tiles the texture
    // across the quad (the textures all wrap with GL_REPEAT).
    const float s = uvScale;
    addTriangle(a, b, c, {0.0f, 0.0f}, {s, 0.0f}, {s, s});
    addTriangle(a, c, d, {0.0f, 0.0f}, {s, s}, {0.0f, s});
}

// --- internal helpers -------------------------------------------------------

namespace {

// Axis-aligned box from min corner to max corner, six quads, normals out.
// Vertex order on each face is CCW viewed from outside that face, so the
// cross product in addTriangle lands on the outward normal.
void addBox(MeshData& md, const glm::vec3& mn, const glm::vec3& mx) {
    // +z (front) / -z (back)
    md.addQuad({mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z});
    md.addQuad({mx.x, mn.y, mn.z}, {mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z});
    // +x (right) / -x (left)
    md.addQuad({mx.x, mn.y, mx.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z});
    md.addQuad({mn.x, mn.y, mn.z}, {mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z});
    // +y (top) / -y (bottom)
    md.addQuad({mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z});
    md.addQuad({mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z});
}

// Translate every vertex so the mesh's AABB center sits at the local origin.
// Pure translation — normals and UVs are unaffected. Used by the architectural
// primitives so Transform.position maps to the object's visual center (the
// convention DCC tools and the MCP agent assume).
void centerMeshData(MeshData& md) {
    if (md.vertices.empty()) return;
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    for (size_t i = 0; i < md.vertices.size(); i += 8) {
        glm::vec3 p(md.vertices[i], md.vertices[i + 1], md.vertices[i + 2]);
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    const glm::vec3 c = (mn + mx) * 0.5f;
    for (size_t i = 0; i < md.vertices.size(); i += 8) {
        md.vertices[i]     -= c.x;
        md.vertices[i + 1] -= c.y;
        md.vertices[i + 2] -= c.z;
    }
}

// Same deterministic hash family as Texture's noise — blob shapes must be
// reproducible from their seed so a recurring dream object keeps its lumps.
using render_detail::hashU32;

float hash01(std::uint32_t seed, std::uint32_t index) {
    std::uint32_t h = hashU32(index * 0x9E3779B9U ^ hashU32(seed));
    return static_cast<float>(h & 0x00FFFFFFU) / 16777216.0f;
}

constexpr float kTau = 6.28318530717958647692f;

// Once-subdivided icosphere (42 verts, 80 faces), displaced per unique vertex
// so the surface stays watertight, then flattened to per-face duplicates for
// hard facets. Shared by blob (round body), tree canopies, and rocks — they
// differ only in radius/bumpiness/squash/placement.
void appendIcosphere(MeshData& md, unsigned int seed, float radius, float bump,
                     float yScale, const glm::vec3& center) {
    const float t = (1.0f + 2.2360679f) * 0.5f; // golden ratio, sqrt(5) inline

    std::vector<glm::vec3> verts = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
    };
    for (glm::vec3& v : verts) {
        v = glm::normalize(v);
    }

    // Icosahedron faces, CCW viewed from outside.
    std::vector<std::array<int, 3>> faces = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
    };

    // One subdivision: split each edge at its midpoint (re-projected onto the
    // unit sphere), 1 face -> 4. The midpoint cache keyed on the edge's
    // vertex pair guarantees both faces sharing an edge get the *same*
    // midpoint vertex — that shared identity is what keeps the displaced
    // surface watertight.
    std::unordered_map<std::uint64_t, int> midpointCache;
    auto midpoint = [&](int i, int j) -> int {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(std::min(i, j)) << 32) |
            static_cast<std::uint32_t>(std::max(i, j));
        auto it = midpointCache.find(key);
        if (it != midpointCache.end()) {
            return it->second;
        }
        glm::vec3 m = glm::normalize((verts[i] + verts[j]) * 0.5f);
        int index = static_cast<int>(verts.size());
        verts.push_back(m);
        midpointCache.emplace(key, index);
        return index;
    };

    std::vector<std::array<int, 3>> subdivided;
    subdivided.reserve(faces.size() * 4);
    for (const auto& f : faces) {
        int ab = midpoint(f[0], f[1]);
        int bc = midpoint(f[1], f[2]);
        int ca = midpoint(f[2], f[0]);
        subdivided.push_back({f[0], ab, ca});
        subdivided.push_back({f[1], bc, ab});
        subdivided.push_back({f[2], ca, bc});
        subdivided.push_back({ab, bc, ca});
    }

    // Displace each unique vertex along its (spherical) normal — for a unit
    // sphere centered at origin the normal *is* the position — then squash
    // vertically and move into place.
    std::vector<glm::vec3> displaced(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        const float r = radius + hash01(seed, static_cast<std::uint32_t>(i)) * bump;
        glm::vec3 p = verts[i] * r;
        p.y *= yScale;
        displaced[i] = p + center;
    }

    // Only now flatten to per-face duplicated vertices: addTriangle computes
    // the face normal from the *displaced* positions, which is what makes the
    // lumps read as hard facets instead of a smooth balloon.
    const glm::vec2 uv(0.5f, 0.5f); // texture is white; uv is a formality
    for (const auto& f : subdivided) {
        md.addTriangle(displaced[f[0]], displaced[f[1]], displaced[f[2]], uv, uv, uv);
    }
}

// Untwisted n-gon frustum/cone under an arbitrary transform: rings at y=0 and
// y=height (collapsed to an apex when rTop ~ 0), every point pushed through
// `xf`. The transform is what lets crystals tilt and structure cones stretch
// non-uniformly without this helper knowing about either.
void addTaperedPrism(MeshData& md, const glm::mat4& xf, int sides, float rBase,
                     float rTop, float height, float phase) {
    sides = std::clamp(sides, 3, 8);
    const bool pointed = (rTop < 0.02f);

    auto ring = [&](float radius, float y) {
        std::vector<glm::vec3> pts(static_cast<size_t>(sides));
        for (int i = 0; i < sides; ++i) {
            const float a = phase + kTau * (static_cast<float>(i) / sides);
            pts[static_cast<size_t>(i)] = glm::vec3(
                xf * glm::vec4(std::cos(a) * radius, y, std::sin(a) * radius, 1.0f));
        }
        return pts;
    };

    const std::vector<glm::vec3> bottom = ring(rBase, 0.0f);
    const glm::vec3 apex = glm::vec3(xf * glm::vec4(0.0f, height, 0.0f, 1.0f));
    const std::vector<glm::vec3> top =
        pointed ? std::vector<glm::vec3>{} : ring(rTop, height);

    const glm::vec2 uv(0.5f, 0.5f);
    for (int i = 0; i < sides; ++i) {
        const int j = (i + 1) % sides;
        const glm::vec3& b0 = bottom[static_cast<size_t>(i)];
        const glm::vec3& b1 = bottom[static_cast<size_t>(j)];
        if (pointed) {
            md.addTriangle(b0, b1, apex, uv, uv, uv);
        } else {
            const glm::vec3& t0 = top[static_cast<size_t>(i)];
            const glm::vec3& t1 = top[static_cast<size_t>(j)];
            md.addTriangle(b0, b1, t1, uv, uv, uv);
            md.addTriangle(b0, t1, t0, uv, uv, uv);
        }
    }
    // Bottom cap (fan), wound so its normal points down.
    for (int i = 1; i < sides - 1; ++i) {
        md.addTriangle(bottom[0], bottom[static_cast<size_t>(i + 1)],
                       bottom[static_cast<size_t>(i)], uv, uv, uv);
    }
    if (!pointed) {
        for (int i = 1; i < sides - 1; ++i) {
            md.addTriangle(top[0], top[static_cast<size_t>(i)],
                           top[static_cast<size_t>(i + 1)], uv, uv, uv);
        }
    }
}

} // namespace

// --- Mesh: GL object lifetime ----------------------------------------------

Mesh::Mesh(const MeshData& data) {
    m_vertexCount = static_cast<int>(data.vertices.size() / 8);

    // Local-space AABB straight from the positions. The game uses this for
    // collision and raycasts, so it must reflect the *generated* geometry
    // (post-displacement for blobs), not any idealized shape.
    if (m_vertexCount > 0) {
        localMin = glm::vec3(data.vertices[0], data.vertices[1], data.vertices[2]);
        localMax = localMin;
        for (int i = 0; i < m_vertexCount; ++i) {
            const float* v = &data.vertices[static_cast<size_t>(i) * 8];
            glm::vec3 p(v[0], v[1], v[2]);
            localMin = glm::min(localMin, p);
            localMax = glm::max(localMax, p);
        }
    }

    // One VAO per mesh. The VAO remembers the buffer binding + attribute
    // layout, so draw() is just "bind VAO, draw" — no per-frame attribute
    // re-specification. Core profile *requires* a VAO; there is no default
    // one like in compatibility GL.
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // GL_STATIC_DRAW: written once here, drawn many times, never read back.
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.vertices.size() * sizeof(float)),
                 data.vertices.data(), GL_STATIC_DRAW);

    // Interleaved layout, 8 floats per vertex: pos3 | normal3 | uv2.
    // Attribute indices are a contract with the vertex shader's
    // layout(location = N) declarations.
    const GLsizei stride = 8 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // Unbind the VAO first — unbinding GL_ARRAY_BUFFER while the VAO is bound
    // would be fine (the attribute pointers already captured the buffer), but
    // unbinding in this order means no later code can accidentally scribble
    // attribute state into this VAO.
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

Mesh::~Mesh() {
    if (m_vbo != 0) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (m_vao != 0) {
        glDeleteVertexArrays(1, &m_vao);
    }
}

Mesh::Mesh(Mesh&& other) noexcept
    : localMin(other.localMin),
      localMax(other.localMax),
      m_vao(other.m_vao),
      m_vbo(other.m_vbo),
      m_vertexCount(other.m_vertexCount) {
    other.m_vao = 0;
    other.m_vbo = 0;
    other.m_vertexCount = 0;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (this != &other) {
        if (m_vbo != 0) {
            glDeleteBuffers(1, &m_vbo);
        }
        if (m_vao != 0) {
            glDeleteVertexArrays(1, &m_vao);
        }
        localMin = other.localMin;
        localMax = other.localMax;
        m_vao = other.m_vao;
        m_vbo = other.m_vbo;
        m_vertexCount = other.m_vertexCount;
        other.m_vao = 0;
        other.m_vbo = 0;
        other.m_vertexCount = 0;
    }
    return *this;
}

void Mesh::draw() const {
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
}

// --- Primitive factories -----------------------------------------------------

Mesh Mesh::box() {
    MeshData md;
    addBox(md, {-0.5f, 0.0f, -0.5f}, {0.5f, 1.0f, 0.5f});
    centerMeshData(md);
    return Mesh(md);
}

Mesh Mesh::pyramid() {
    MeshData md;
    // Built foot-on-y=0, then centerMeshData() shifts the whole mesh down so
    // the AABB midpoint (y=0.5) lands on the origin.
    const glm::vec3 apex(0.0f, 1.0f, 0.0f);
    // Base corners, y=0 (pre-centering).
    const glm::vec3 fl(-0.5f, 0.0f, 0.5f);  // front-left  (+z)
    const glm::vec3 fr(0.5f, 0.0f, 0.5f);   // front-right
    const glm::vec3 br(0.5f, 0.0f, -0.5f);  // back-right  (-z)
    const glm::vec3 bl(-0.5f, 0.0f, -0.5f); // back-left

    // Four sides, each wound CCW seen from outside so normals lean outward.
    const glm::vec2 uv0(0.0f, 0.0f), uv1(1.0f, 0.0f), uvTop(0.5f, 1.0f);
    md.addTriangle(fl, fr, apex, uv0, uv1, uvTop); // +z
    md.addTriangle(fr, br, apex, uv0, uv1, uvTop); // +x
    md.addTriangle(br, bl, apex, uv0, uv1, uvTop); // -z
    md.addTriangle(bl, fl, apex, uv0, uv1, uvTop); // -x
    // Base, facing down.
    md.addQuad(bl, br, fr, fl);
    centerMeshData(md);
    return Mesh(md);
}

Mesh Mesh::pillar() {
    MeshData md;
    addBox(md, {-0.2f, 0.0f, -0.2f}, {0.2f, 2.4f, 0.2f});
    centerMeshData(md);
    return Mesh(md);
}

Mesh Mesh::arch() {
    // Two 0.3-wide pillars centered at x = +/-0.85 (outer edges at +/-1.0),
    // 2.0 tall, plus a lintel box bridging the gap on top. One mesh, one
    // draw call — the dream doesn't need an articulated arch. Built foot-on-y=0
    // below, then centerMeshData() recenters it on the AABB midpoint.
    MeshData md;
    const float halfW = 0.15f;  // pillar half-width (0.3 wide in x and z)
    const float height = 2.0f;
    addBox(md, {-0.85f - halfW, 0.0f, -halfW}, {-0.85f + halfW, height, halfW});
    addBox(md, {0.85f - halfW, 0.0f, -halfW}, {0.85f + halfW, height, halfW});
    // Lintel: 2.0 x 0.35 x 0.3, sitting on the pillar tops at y=2.0.
    addBox(md, {-1.0f, height, -halfW}, {1.0f, height + 0.35f, halfW});
    centerMeshData(md);
    return Mesh(md);
}

Mesh Mesh::blob(unsigned int seed) {
    // The 0.5-radius lumpy body centered at y=0.6 keeps the underside near
    // (but not necessarily exactly on) the ground.
    MeshData md;
    appendIcosphere(md, seed, 0.5f, 0.18f, 1.0f, {0.0f, 0.6f, 0.0f});
    return Mesh(md);
}

Mesh Mesh::stair() {
    // Five full-height boxes rather than an optimized staircase shell: the
    // hidden faces are a handful of triangles, and solid boxes mean the AABB
    // and any future per-step collision stay dead simple. Built foot-on-y=0
    // and z starting at 0, then centerMeshData() recenters on the AABB.
    MeshData md;
    const float halfW = kStairHalfWidth; // 1.4 wide in x
    const float rise = kStairRise;
    const float run = kStairRun;
    for (int i = 0; i < kStairSteps; ++i) {
        addBox(md,
               {-halfW, 0.0f, static_cast<float>(i) * run},
               {halfW, static_cast<float>(i + 1) * rise, static_cast<float>(i + 1) * run});
    }
    centerMeshData(md);
    return Mesh(md);
}

Mesh Mesh::form(int sides, float twist, float taper, unsigned int seed) {
    // A parametric n-gon prism — the one shape the LLM authors freely. The six
    // fixed primitives are hand-tuned; this one is a generator, so it clamps
    // every input into a range that can't produce a degenerate or NaN mesh.
    sides = std::clamp(sides, 3, 8);
    taper = std::clamp(taper, 0.0f, 1.0f);
    twist = std::clamp(twist, 0.0f, 0.9f);

    // Built bottom-ring-on-y=0 below; centerMeshData() at the end shifts the
    // whole prism so its AABB midpoint sits on the local origin.
    const float baseR = 0.5f;             // bottom-ring radius
    const float topR = baseR * taper;     // 0 -> apex (cone)
    const float height = 1.6f;            // before the object's own scale
    const bool pointed = (topR < 0.02f);  // collapse the top ring to a point
    // twist 0.9 -> ~120deg of accumulated rotation from base to top.
    const float twistRad = twist * (kTau / 3.0f);
    // A tiny seeded angular phase so two forms with identical params don't read
    // as the exact same object when they share an area.
    const float phase = hash01(seed, 0u) * kTau;

    auto ring = [&](float radius, float y, float extraRot) {
        std::vector<glm::vec3> pts(static_cast<size_t>(sides));
        for (int i = 0; i < sides; ++i) {
            const float a = phase + extraRot + kTau * (static_cast<float>(i) / sides);
            pts[static_cast<size_t>(i)] = glm::vec3(std::cos(a) * radius, y,
                                                    std::sin(a) * radius);
        }
        return pts;
    };

    const std::vector<glm::vec3> bottom = ring(baseR, 0.0f, 0.0f);
    const glm::vec3 apex(0.0f, height, 0.0f);
    const std::vector<glm::vec3> top =
        pointed ? std::vector<glm::vec3>{} : ring(topR, height, twistRad);

    MeshData md;
    const glm::vec2 uv(0.5f, 0.5f); // white texture; uv is a formality

    // Sides. Each face is a quad (bottom[i]-bottom[i+1]-top[i+1]-top[i]) wound
    // CCW from outside, or a triangle up to the apex when the top collapses.
    for (int i = 0; i < sides; ++i) {
        const int j = (i + 1) % sides;
        const glm::vec3& b0 = bottom[static_cast<size_t>(i)];
        const glm::vec3& b1 = bottom[static_cast<size_t>(j)];
        if (pointed) {
            md.addTriangle(b0, b1, apex, uv, uv, uv);
        } else {
            const glm::vec3& t0 = top[static_cast<size_t>(i)];
            const glm::vec3& t1 = top[static_cast<size_t>(j)];
            md.addTriangle(b0, b1, t1, uv, uv, uv);
            md.addTriangle(b0, t1, t0, uv, uv, uv);
        }
    }

    // Bottom cap (fan from vertex 0), wound so its normal points down (-y).
    for (int i = 1; i < sides - 1; ++i) {
        md.addTriangle(bottom[0], bottom[static_cast<size_t>(i + 1)],
                       bottom[static_cast<size_t>(i)], uv, uv, uv);
    }
    // Top cap, only when the top is a real ring (a cone needs none).
    if (!pointed) {
        for (int i = 1; i < sides - 1; ++i) {
            md.addTriangle(top[0], top[static_cast<size_t>(i)],
                           top[static_cast<size_t>(i + 1)], uv, uv, uv);
        }
    }
    centerMeshData(md);
    return Mesh(md);
}

Mesh Mesh::tree(unsigned int seed, std::vector<PartBox>* outBoxes) {
    // Trunk: a gently tapered 6-gon column. Canopy: the seed picks between a
    // lumpy round crown (one displaced icosphere) and a pine (stacked cones).
    // The vertical gradient convention is color = trunk, color2 = crown — the
    // crown sits entirely above the trunk so the gradient lands on the split.
    MeshData md;
    const float phase = hash01(seed, 0u) * kTau;
    const float trunkH = 1.3f;
    addTaperedPrism(md, glm::mat4(1.0f), 6, 0.14f, 0.08f, trunkH, phase);

    // The trunk blocks; the crown does not. Boxes mirror the geometry below.
    if (outBoxes) {
        outBoxes->push_back(PartBox{{-0.16f, 0.0f, -0.16f},
                                    {0.16f, trunkH, 0.16f},
                                    /*walkable=*/false, /*solid=*/true});
    }

    if (hashU32(seed) & 1u) {
        // Round crown, sunk slightly onto the trunk top.
        appendIcosphere(md, seed, 0.55f, 0.12f, 0.9f, {0.0f, trunkH + 0.35f, 0.0f});
        if (outBoxes) {
            const float r = 0.55f + 0.12f; // radius + max displacement
            outBoxes->push_back(PartBox{{-r, trunkH + 0.35f - r, -r},
                                        {r, trunkH + 0.35f + r, r},
                                        /*walkable=*/false, /*solid=*/false});
        }
    } else {
        // Pine: three stacked cones, each narrower than the last.
        const float r[3] = {0.55f, 0.42f, 0.28f};
        float y = trunkH * 0.55f;
        const float y0 = y;
        for (int i = 0; i < 3; ++i) {
            const glm::mat4 xf = glm::translate(glm::mat4(1.0f), {0.0f, y, 0.0f});
            addTaperedPrism(md, xf, 6, r[i], 0.0f, 0.65f, phase + 0.3f * i);
            y += 0.42f;
        }
        if (outBoxes) {
            // y is now the base of the (never-placed) 4th cone; the last
            // placed cone tops out 0.65 above its base at y - 0.42.
            outBoxes->push_back(PartBox{{-r[0], y0, -r[0]},
                                        {r[0], y - 0.42f + 0.65f, r[0]},
                                        /*walkable=*/false, /*solid=*/false});
        }
    }
    return Mesh(md);
}

Mesh Mesh::rock(unsigned int seed) {
    // A squashed, heavily bumped icosphere. Centered low enough that the
    // lumpy underside sits just below the ground — buried, like rocks are.
    MeshData md;
    appendIcosphere(md, seed, 0.5f, 0.35f, 0.55f, {0.0f, 0.42f, 0.0f});
    return Mesh(md);
}

Mesh Mesh::crystal(unsigned int seed) {
    // A cluster of 2-3 pointed prisms leaning out of the ground at seeded
    // tilts. Each shard's base sits a touch below y=0 so the tilt never
    // leaves a corner hovering.
    MeshData md;
    const int shards = 2 + static_cast<int>(hashU32(seed ^ 0x51u) % 2u);
    for (int i = 0; i < shards; ++i) {
        const std::uint32_t s = static_cast<std::uint32_t>(i);
        const float h = 0.7f + hash01(seed, s * 3u + 1u) * 0.9f;       // 0.7..1.6
        const float r = 0.16f + hash01(seed, s * 3u + 2u) * 0.10f;     // 0.16..0.26
        const float ang = hash01(seed, s * 3u + 3u) * kTau;            // offset dir
        const float off = (i == 0) ? 0.0f : 0.25f + hash01(seed, s + 9u) * 0.55f;
        const float tilt = glm::radians((hash01(seed, s + 17u) - 0.5f) * 30.0f);
        const int sides = 5 + static_cast<int>(hashU32(seed ^ (0xC0DEu + s)) % 2u);

        glm::mat4 xf = glm::translate(
            glm::mat4(1.0f),
            {std::cos(ang) * off, -0.08f, std::sin(ang) * off});
        xf = glm::rotate(xf, ang, glm::vec3(0.0f, 1.0f, 0.0f));
        xf = glm::rotate(xf, tilt, glm::vec3(1.0f, 0.0f, 0.0f));
        addTaperedPrism(md, xf, sides, r, 0.0f, h, hash01(seed, s + 23u) * kTau);
    }
    return Mesh(md);
}

Mesh Mesh::structure(const std::vector<MeshPart>& parts, unsigned int seed,
                     std::vector<PartBox>* outBoxes) {
    // The LLM's floor plan, made solid. Each part is a primitive dropped at
    // its base-center in structure-local units; the assembled whole is then
    // recentered on x/z, sunk to y=0, and normalized so the footprint never
    // exceeds ~4.5 units — the object's own scale stays in charge of size,
    // and a wild parts list can't dwarf the field. The seed only varies cone
    // facing; the floor plan itself is the model's, verbatim.
    MeshData md;
    const float phase = hash01(seed, 0u) * kTau;

    // Collision boxes mirror the geometry emitted below, piece by piece, so
    // whatever the player can see is exactly what blocks them.
    std::vector<PartBox> boxes;
    auto solid = [&](const glm::vec3& mn, const glm::vec3& mx, bool walkable = false) {
        boxes.push_back(PartBox{mn, mx, walkable});
    };

    for (const MeshPart& part : parts) {
        const glm::vec3& at = part.at;
        glm::vec3 size = glm::max(part.size, glm::vec3(0.1f));
        switch (part.kind) {
            case MeshPart::Kind::Slab:
                size.y *= 0.25f;
                [[fallthrough]];
            case MeshPart::Kind::Box:
                addBox(md, {at.x - size.x * 0.5f, at.y, at.z - size.z * 0.5f},
                       {at.x + size.x * 0.5f, at.y + size.y, at.z + size.z * 0.5f});
                solid({at.x - size.x * 0.5f, at.y, at.z - size.z * 0.5f},
                      {at.x + size.x * 0.5f, at.y + size.y, at.z + size.z * 0.5f},
                      /*walkable=*/true); // flat tops: floors, decks, plinths
                break;
            case MeshPart::Kind::Wall: {
                // Thin out the smaller horizontal extent: the bigger one is
                // the wall's run, the smaller its thickness.
                if (size.x >= size.z) size.z = 0.3f; else size.x = 0.3f;
                addBox(md, {at.x - size.x * 0.5f, at.y, at.z - size.z * 0.5f},
                       {at.x + size.x * 0.5f, at.y + size.y, at.z + size.z * 0.5f});
                solid({at.x - size.x * 0.5f, at.y, at.z - size.z * 0.5f},
                      {at.x + size.x * 0.5f, at.y + size.y, at.z + size.z * 0.5f});
                break;
            }
            case MeshPart::Kind::Pillar:
                addBox(md, {at.x - 0.175f, at.y, at.z - 0.175f},
                       {at.x + 0.175f, at.y + size.y, at.z + 0.175f});
                solid({at.x - 0.175f, at.y, at.z - 0.175f},
                      {at.x + 0.175f, at.y + size.y, at.z + 0.175f});
                break;
            case MeshPart::Kind::Roof: {
                // Gabled: ridge runs along the longer horizontal extent, eave
                // overhang baked into the footprint the model gave us.
                const bool ridgeX = (size.x >= size.z);
                const float hw = size.x * 0.5f, hd = size.z * 0.5f;
                const float ry = at.y + size.y;
                const glm::vec3 e0{at.x - hw, at.y, at.z - hd};
                const glm::vec3 e1{at.x + hw, at.y, at.z - hd};
                const glm::vec3 e2{at.x + hw, at.y, at.z + hd};
                const glm::vec3 e3{at.x - hw, at.y, at.z + hd};
                const glm::vec3 r0 = ridgeX ? glm::vec3{at.x - hw, ry, at.z}
                                            : glm::vec3{at.x, ry, at.z - hd};
                const glm::vec3 r1 = ridgeX ? glm::vec3{at.x + hw, ry, at.z}
                                            : glm::vec3{at.x, ry, at.z + hd};
                const glm::vec2 uv0(0.0f, 0.0f), uv1(1.0f, 0.0f),
                    uv2(1.0f, 1.0f), uv3(0.0f, 1.0f);
                if (ridgeX) {
                    // Slopes (quads), wound CCW from outside.
                    md.addTriangle(e0, e1, r1, uv0, uv1, uv2);
                    md.addTriangle(e0, r1, r0, uv0, uv2, uv3);
                    md.addTriangle(e2, e3, r0, uv0, uv1, uv2);
                    md.addTriangle(e2, r0, r1, uv0, uv2, uv3);
                    // Gable triangles at the ends.
                    md.addTriangle(e3, e0, r0, uv0, uv1, uv2);
                    md.addTriangle(e1, e2, r1, uv0, uv1, uv2);
                } else {
                    md.addTriangle(e1, e2, r1, uv0, uv1, uv2);
                    md.addTriangle(e1, r1, r0, uv0, uv2, uv3);
                    md.addTriangle(e3, e0, r0, uv0, uv1, uv2);
                    md.addTriangle(e3, r0, r1, uv0, uv2, uv3);
                    md.addTriangle(e0, e1, r0, uv0, uv1, uv2);
                    md.addTriangle(e2, e3, r1, uv0, uv1, uv2);
                }
                // Underside, so the AABB volume reads solid from below.
                md.addQuad(e0, e3, e2, e1);
                solid({at.x - hw, at.y, at.z - hd},
                      {at.x + hw, at.y + size.y, at.z + hd});
                break;
            }
            case MeshPart::Kind::Cone: {
                glm::mat4 xf = glm::translate(glm::mat4(1.0f), at);
                xf = glm::scale(xf, {size.x, 1.0f, size.z});
                addTaperedPrism(md, xf, 6, 0.5f, 0.0f, size.y, phase);
                // Base ring radius 0.5 scaled by size.x/size.z.
                solid({at.x - size.x * 0.5f, at.y, at.z - size.z * 0.5f},
                      {at.x + size.x * 0.5f, at.y + size.y, at.z + size.z * 0.5f});
                break;
            }
            case MeshPart::Kind::Arch: {
                // Two jambs + lintel spanning size.x, like Mesh::arch but
                // sized by the part.
                const float halfSpan = size.x * 0.5f;
                const float jw = std::min(0.3f, size.x * 0.2f); // jamb width
                const float hd = std::max(size.z * 0.5f, 0.15f);
                addBox(md, {at.x - halfSpan, at.y, at.z - hd},
                       {at.x - halfSpan + jw, at.y + size.y, at.z + hd});
                addBox(md, {at.x + halfSpan - jw, at.y, at.z - hd},
                       {at.x + halfSpan, at.y + size.y, at.z + hd});
                addBox(md, {at.x - halfSpan, at.y + size.y, at.z - hd},
                       {at.x + halfSpan, at.y + size.y + 0.35f, at.z + hd});
                // Three solids — jambs and lintel — so the opening stays open.
                solid({at.x - halfSpan, at.y, at.z - hd},
                      {at.x - halfSpan + jw, at.y + size.y, at.z + hd});
                solid({at.x + halfSpan - jw, at.y, at.z - hd},
                      {at.x + halfSpan, at.y + size.y, at.z + hd});
                solid({at.x - halfSpan, at.y + size.y, at.z - hd},
                      {at.x + halfSpan, at.y + size.y + 0.35f, at.z + hd});
                break;
            }
        }
    }

    if (md.vertices.empty()) {
        // A structure with no usable parts still has to be *something*.
        addBox(md, {-1.0f, 0.0f, -1.0f}, {1.0f, 1.5f, 1.0f});
        solid({-1.0f, 0.0f, -1.0f}, {1.0f, 1.5f, 1.0f});
    }

    // Normalize in place: recenter x/z, sink the base to y=0, and uniformly
    // shrink anything whose footprint exceeds the budget. Translation and
    // uniform scale leave the baked normals valid, so only positions move.
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    const size_t n = md.vertices.size() / 8;
    for (size_t i = 0; i < n; ++i) {
        const float* v = &md.vertices[i * 8];
        mn = glm::min(mn, glm::vec3(v[0], v[1], v[2]));
        mx = glm::max(mx, glm::vec3(v[0], v[1], v[2]));
    }
    const glm::vec3 center{(mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f};
    const float footprint = std::max(mx.x - mn.x, mx.z - mn.z);
    const float shrink = footprint > 4.5f ? 4.5f / footprint : 1.0f;
    for (size_t i = 0; i < n; ++i) {
        float* v = &md.vertices[i * 8];
        v[0] = (v[0] - center.x) * shrink;
        v[1] = (v[1] - center.y) * shrink;
        v[2] = (v[2] - center.z) * shrink;
    }
    // The collision boxes ride through the same normalization so they keep
    // matching the vertices exactly.
    for (PartBox& b : boxes) {
        b.mn = (b.mn - center) * shrink;
        b.mx = (b.mx - center) * shrink;
    }
    if (outBoxes) *outBoxes = std::move(boxes);
    return Mesh(md);
}

Mesh Mesh::groundPlane(float halfSize, float uvTiles) {
    // Tessellated grid, not one big quad. Affine UV interpolation warps in
    // proportion to the depth range a single triangle spans, so a plane-sized
    // triangle under the camera swims violently — the PS1 answer was to
    // subdivide ground meshes, and we do the same (~3 world units per cell,
    // matching TerrainField's grid). Fog + low draw distance still hide the
    // edges; tiling the uvs keeps texel density chunky like the props.
    MeshData md;
    const int   cells = std::max(1, static_cast<int>(2.0f * halfSize / 3.0f));
    const float step  = 2.0f * halfSize / cells;
    const float uvStep = uvTiles / cells;
    for (int iz = 0; iz < cells; ++iz) {
        for (int ix = 0; ix < cells; ++ix) {
            const float x0 = -halfSize + ix * step, x1 = x0 + step;
            const float z0 = -halfSize + iz * step, z1 = z0 + step;
            const float u0 = ix * uvStep, u1 = u0 + uvStep;
            const float v0 = iz * uvStep, v1 = v0 + uvStep;
            const glm::vec3 a{x0, 0.0f, z1}, b{x1, 0.0f, z1};
            const glm::vec3 c{x1, 0.0f, z0}, d{x0, 0.0f, z0};
            md.addTriangle(a, b, c, {u0, v0}, {u1, v0}, {u1, v1});
            md.addTriangle(a, c, d, {u0, v0}, {u1, v1}, {u0, v1});
        }
    }
    return Mesh(md);
}

Mesh Mesh::quad() {
    // A single 1x1 quad standing in the XY plane (z=0), centered on the origin
    // so its corners run (-0.5,-0.5,0)..(0.5,0.5,0), facing +z. This breaks the
    // "base on y=0" convention on purpose: it's the billboard/decal sprite, and
    // the game orients it (camera-facing billboard, or flat-on-a-wall decal)
    // entirely through the model matrix — centering on the origin keeps that
    // rotation pivot in the middle of the sprite. UVs span the full 0..1 texture
    // (no tiling) so alpha-test discard cuts the sprite's shape cleanly out of
    // one texture. Subdivided 4x4: ground decals are viewed at grazing angles
    // right under the camera, where a single-quad affine warp smears the sprite
    // sideways — smaller triangles bound the per-triangle depth span.
    MeshData md;
    constexpr int   kDiv  = 4;
    constexpr float kStep = 1.0f / kDiv;
    for (int iy = 0; iy < kDiv; ++iy) {
        for (int ix = 0; ix < kDiv; ++ix) {
            const float u0 = ix * kStep, u1 = u0 + kStep;
            const float v0 = iy * kStep, v1 = v0 + kStep;
            const glm::vec3 a{u0 - 0.5f, v0 - 0.5f, 0.0f};
            const glm::vec3 b{u1 - 0.5f, v0 - 0.5f, 0.0f};
            const glm::vec3 c{u1 - 0.5f, v1 - 0.5f, 0.0f};
            const glm::vec3 d{u0 - 0.5f, v1 - 0.5f, 0.0f};
            md.addTriangle(a, b, c, {u0, v0}, {u1, v0}, {u1, v1});
            md.addTriangle(a, c, d, {u0, v0}, {u1, v1}, {u0, v1});
        }
    }
    return Mesh(md);
}

Mesh Mesh::plane() {
    // A thin horizontal slab: 1x1 footprint in x/z, 0.1 thick. A solid box (not
    // a single quad) so the AABB is a real volume the collision system can stand
    // on, and so the slab reads as a physical platform from any angle rather
    // than vanishing edge-on. centerMeshData() recenters it on the AABB, so the
    // origin lands mid-slab (top at +0.05, bottom at -0.05). It takes no tilt
    // params: the game lifts it to its elevation and bakes any pitch/roll into
    // the model matrix, which keeps this local AABB a clean flat top — exactly
    // what the collision query wants to resolve against.
    MeshData md;
    addBox(md, {-0.5f, 0.0f, -0.5f}, {0.5f, 0.1f, 0.5f});
    centerMeshData(md);
    return Mesh(md);
}

} // namespace liminal
