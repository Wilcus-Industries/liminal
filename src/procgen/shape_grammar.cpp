// shape_grammar.cpp — footprints + grid runs -> boxy low-poly architecture.
//
// Everything is axis-aligned boxes and a few sloped roof quads, emitted in
// world space (draw with an identity model, boxes are exact AABBs). The
// interpreter walks each family's rule parameters; geometry helpers below
// are the terminals of the grammar.

#include <liminal/procgen/shape_grammar.hpp>

#include <liminal/core/assets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <optional>

#include <nlohmann/json.hpp>

namespace liminal::procgen {

namespace {

// --- geometry terminals ----------------------------------------------------

// Axis-aligned box from min corner + size, hard-normal faces, world-space
// UVs (u/v in world units * 0.4 so the theme textures tile evenly).
void addBox(MeshData& md, std::vector<PartBox>& boxes, const glm::vec3& mn,
            const glm::vec3& size, bool walkable = false, bool solid = true) {
    const glm::vec3 mx = mn + size;
    constexpr float uv = 0.4f;
    auto quad = [&md](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                      const glm::vec3& d, const glm::vec2& ua, const glm::vec2& ub,
                      const glm::vec2& uc, const glm::vec2& ud) {
        md.addTriangle(a, b, c, ua, ub, uc);
        md.addTriangle(a, c, d, ua, uc, ud);
    };
    // +y (top)
    quad({mn.x, mx.y, mn.z}, {mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z}, {mx.x, mx.y, mn.z},
         {mn.x * uv, mn.z * uv}, {mn.x * uv, mx.z * uv}, {mx.x * uv, mx.z * uv}, {mx.x * uv, mn.z * uv});
    // -y (bottom)
    quad({mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z},
         {mn.x * uv, mn.z * uv}, {mx.x * uv, mn.z * uv}, {mx.x * uv, mx.z * uv}, {mn.x * uv, mx.z * uv});
    // +x
    quad({mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, {mx.x, mn.y, mx.z},
         {mn.z * uv, mn.y * uv}, {mn.z * uv, mx.y * uv}, {mx.z * uv, mx.y * uv}, {mx.z * uv, mn.y * uv});
    // -x
    quad({mn.x, mn.y, mn.z}, {mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z},
         {mn.z * uv, mn.y * uv}, {mx.z * uv, mn.y * uv}, {mx.z * uv, mx.y * uv}, {mn.z * uv, mx.y * uv});
    // +z
    quad({mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
         {mn.x * uv, mn.y * uv}, {mx.x * uv, mn.y * uv}, {mx.x * uv, mx.y * uv}, {mn.x * uv, mx.y * uv});
    // -z
    quad({mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mn.y, mn.z},
         {mn.x * uv, mn.y * uv}, {mn.x * uv, mx.y * uv}, {mx.x * uv, mx.y * uv}, {mx.x * uv, mn.y * uv});

    PartBox pb;
    pb.mn = mn;
    pb.mx = mx;
    pb.walkable = walkable;
    pb.solid = solid;
    boxes.push_back(pb);
}

// A hole in a wall run: span along the run direction + a vertical band.
struct Opening {
    float center = 0.0f; // along the run, from its start
    float width = 1.0f;
    float bottom = 0.0f; // above the wall's base
    float top = 2.0f;
};

// Wall run from `start`, length `len` along `dir` (unit, +x or +z), base at
// startY, `height` tall, `thick` deep. Openings split it into boxes so the
// holes are genuinely open (doorways walkable, windows seen through).
void addWallRun(MeshData& md, std::vector<PartBox>& boxes, glm::vec3 start,
                const glm::vec3& dir, float len, float height, float thick,
                std::vector<Opening> openings) {
    std::sort(openings.begin(), openings.end(),
              [](const Opening& a, const Opening& b) { return a.center < b.center; });
    const glm::vec3 side = (dir.x != 0.0f) ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);

    auto emit = [&](float from, float to, float y0, float y1) {
        if (to - from < 0.05f || y1 - y0 < 0.05f) return;
        const glm::vec3 mn = start + dir * from - side * (thick * 0.5f) +
                             glm::vec3(0, y0, 0);
        const glm::vec3 size = dir * (to - from) + side * thick +
                               glm::vec3(0, y1 - y0, 0);
        addBox(md, boxes, glm::min(mn, mn + size), glm::abs(size));
    };

    float cursor = 0.0f;
    for (const Opening& o : openings) {
        const float a = std::max(cursor, o.center - o.width * 0.5f);
        const float b = std::min(len, o.center + o.width * 0.5f);
        if (b <= a) continue;
        emit(cursor, a, 0.0f, height);          // solid segment before the hole
        emit(a, b, 0.0f, o.bottom);             // sill / threshold
        emit(a, b, o.top, height);              // header / lintel
        cursor = b;
    }
    emit(cursor, len, 0.0f, height);
}

// Horizontal slab over [corner, corner+(w,d)] at base y, `th` thick, with an
// optional rectangular hole (world xz) cut out — the stairwell. The remnant
// strips are walkable so the upper floor is genuinely standable. Same
// split-around-the-hole idea as addWallRun, one dimension up.
void addSlabWithHole(MeshData& md, std::vector<PartBox>& boxes,
                     const glm::vec2& corner, float w, float d, float y,
                     float th, bool hasHole, glm::vec2 hmn, glm::vec2 hmx) {
    if (hasHole) {
        hmn = glm::max(hmn, corner);
        hmx = glm::min(hmx, corner + glm::vec2(w, d));
        if (hmx.x - hmn.x < 0.1f || hmx.y - hmn.y < 0.1f) hasHole = false;
    }
    if (!hasHole) {
        addBox(md, boxes, {corner.x, y, corner.y}, {w, th, d}, /*walkable=*/true);
        return;
    }
    auto strip = [&](float x0, float z0, float x1, float z1) {
        if (x1 - x0 < 0.05f || z1 - z0 < 0.05f) return;
        addBox(md, boxes, {x0, y, z0}, {x1 - x0, th, z1 - z0}, /*walkable=*/true);
    };
    strip(corner.x, corner.y, hmn.x, corner.y + d);  // west of the hole
    strip(hmx.x, corner.y, corner.x + w, corner.y + d); // east
    strip(hmn.x, corner.y, hmx.x, hmn.y);            // south sliver
    strip(hmn.x, hmx.y, hmx.x, corner.y + d);        // north sliver
}

// Gabled roof over a rect: two sloped quads + end triangles, one slab box
// for collision (close enough; nobody stands on a dream roof).
void addGable(MeshData& md, std::vector<PartBox>& boxes, const glm::vec3& mn,
              float w, float d, float rise) {
    const float yb = mn.y;
    const glm::vec3 a{mn.x, yb, mn.z}, b{mn.x + w, yb, mn.z};
    const glm::vec3 c{mn.x + w, yb, mn.z + d}, e{mn.x, yb, mn.z + d};
    const glm::vec3 r0{mn.x, yb + rise, mn.z + d * 0.5f};
    const glm::vec3 r1{mn.x + w, yb + rise, mn.z + d * 0.5f};
    auto uvOf = [](const glm::vec3& v) { return glm::vec2{v.x * 0.4f, (v.z + v.y) * 0.4f}; };
    // Two slopes (quads) + two gable-end triangles. No culling in the
    // renderer, so winding only matters for the hard normals' lighting.
    md.addTriangle(a, b, r1, uvOf(a), uvOf(b), uvOf(r1)); // south slope
    md.addTriangle(a, r1, r0, uvOf(a), uvOf(r1), uvOf(r0));
    md.addTriangle(c, e, r0, uvOf(c), uvOf(e), uvOf(r0)); // north slope
    md.addTriangle(c, r0, r1, uvOf(c), uvOf(r0), uvOf(r1));
    md.addTriangle(a, r0, e, uvOf(a), uvOf(r0), uvOf(e)); // west gable end
    md.addTriangle(b, c, r1, uvOf(b), uvOf(c), uvOf(r1)); // east gable end
    PartBox pb;
    pb.mn = {mn.x, yb, mn.z};
    pb.mx = {mn.x + w, yb + rise, mn.z + d};
    boxes.push_back(pb);
}

void addPyramidRoof(MeshData& md, std::vector<PartBox>& boxes,
                    const glm::vec3& mn, float w, float d, float rise) {
    const glm::vec3 apex{mn.x + w * 0.5f, mn.y + rise, mn.z + d * 0.5f};
    const glm::vec3 a{mn.x, mn.y, mn.z}, b{mn.x + w, mn.y, mn.z};
    const glm::vec3 c{mn.x + w, mn.y, mn.z + d}, e{mn.x, mn.y, mn.z + d};
    auto uvOf = [](const glm::vec3& v) { return glm::vec2{(v.x + v.z) * 0.4f, v.y * 0.4f}; };
    md.addTriangle(a, b, apex, uvOf(a), uvOf(b), uvOf(apex));
    md.addTriangle(b, c, apex, uvOf(b), uvOf(c), uvOf(apex));
    md.addTriangle(c, e, apex, uvOf(c), uvOf(e), uvOf(apex));
    md.addTriangle(e, a, apex, uvOf(e), uvOf(a), uvOf(apex));
    PartBox pb;
    pb.mn = a;
    pb.mx = {c.x, apex.y, c.z};
    boxes.push_back(pb);
}

} // namespace

void applyFamilyParamsJson(FamilyParams& p, const std::string& path) {
    // Routed through the VFS so a mounted pak serves the overlay; missing in
    // both pak and disk keeps the caller's defaults (a full rule set).
    std::optional<std::string> src = Assets::readFile(path);
    if (!src) return;
    const auto j = nlohmann::json::parse(*src, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::fprintf(stderr, "[grammar] unparseable %s, using builtins\n", path.c_str());
        return;
    }
    auto f = [&j](const char* k, float& out) {
        if (j.contains(k) && j[k].is_number()) out = j[k].get<float>();
    };
    auto i = [&j](const char* k, int& out) {
        if (j.contains(k) && j[k].is_number()) out = j[k].get<int>();
    };
    f("floor_height", p.floorHeight);
    f("wall_thickness", p.wallThickness);
    f("foundation", p.foundation);
    i("min_floors", p.minFloors);
    i("max_floors", p.maxFloors);
    if (j.contains("roof") && j["roof"].is_string()) p.roof = j["roof"].get<std::string>();
    f("window_every", p.windowEvery);
    f("window_w", p.windowW);
    f("window_h", p.windowH);
    f("window_sill", p.windowSill);
    f("door_w", p.doorW);
    f("door_h", p.doorH);
    f("inset_per_floor", p.insetPerFloor);
    f("ruin_chaos", p.ruinChaos);
    if (j.contains("colonnade") && j["colonnade"].is_boolean())
        p.colonnade = j["colonnade"].get<bool>();
    if (j.contains("stairs") && j["stairs"].is_boolean())
        p.stairs = j["stairs"].get<bool>();
    f("lamp_chance", p.lampChance);
}

namespace {

// Ground height under a tile rect: the max corner height, so the foundation
// always clears the terrain (no floating corners on a slope).
float rectGroundY(const HeightField& hf, int x0, int z0, int w, int d) {
    float y = -1000.0f;
    for (int z = z0; z <= z0 + d && z < hf.nodes; ++z) {
        for (int x = x0; x <= x0 + w && x < hf.nodes; ++x) {
            y = std::max(y, hf.heights[hf.index(x, z)]);
        }
    }
    return y;
}

} // namespace

// --- building expansion ------------------------------------------------------
// building -> foundation, floors(1..N), roof ; floor -> 4 wall runs with
// window slots (+ the door slot on the ground floor's door side).
BuiltPiece expandBuilding(const TileGrid& g, const HeightField& terrain,
                          const FootprintPlan& fp, const FamilyParams& p,
                          Rng& rng) {
    BuiltPiece out;
    const float ts = g.tileSize;
    // World rect of the wall centerline ring (ring tile centers).
    const glm::vec2 mnT = g.center(fp.x0, fp.z0);
    const float bw = (fp.w - 1) * ts; // centerline extents
    const float bd = (fp.d - 1) * ts;
    const float baseY = rectGroundY(terrain, fp.x0, fp.z0, fp.w, fp.d);

    MeshData md;
    std::vector<PartBox> boxes;

    // Foundation slab: covers the full footprint, top = walking floor.
    const float floorY = baseY + p.foundation;
    addBox(md, boxes,
           {mnT.x - ts * 0.5f, baseY - 0.6f, mnT.y - ts * 0.5f},
           {fp.w * ts, p.foundation + 0.6f, fp.d * ts}, /*walkable=*/true);

    const int floors = rng.range(p.minFloors, p.maxFloors);

    // The door: which side of the ring the validator's tile sits on, and how
    // far along that side, in wall-run coordinates.
    enum Side { South, North, West, East };
    Side doorSide = South;
    float doorAlong = bw * 0.5f;
    {
        const glm::vec2 doorC = g.center(fp.doorX, fp.doorZ);
        if (fp.doorZ == fp.z0) {
            doorSide = South;
            doorAlong = doorC.x - mnT.x;
        } else if (fp.doorZ == fp.z0 + fp.d - 1) {
            doorSide = North;
            doorAlong = doorC.x - mnT.x;
        } else if (fp.doorX == fp.x0) {
            doorSide = West;
            doorAlong = doorC.y - mnT.y;
        } else {
            doorSide = East;
            doorAlong = doorC.y - mnT.y;
        }
    }
    // Keep the whole opening (plus a jamb) inside its run: a door tile at or
    // near a corner would otherwise clamp to a half-width slot with the
    // perpendicular wall run crossing it — unenterable.
    {
        const float runLen = (doorSide == South || doorSide == North) ? bw : bd;
        const float lo = p.doorW * 0.5f + p.wallThickness;
        const float hi = runLen - p.doorW * 0.5f - p.wallThickness;
        doorAlong = (lo <= hi) ? std::clamp(doorAlong, lo, hi) : runLen * 0.5f;
    }

    // Entry steps when the foundation lip at the door overtops the player's
    // 0.5 step-up: baseY is the rect's MAX corner, so on a slope the door-side
    // terrain can sit far below the slab. Walkable boxes descend from the slab
    // edge to the ground outside the door, every rise within step-up reach.
    {
        int ox = fp.doorX, oz = fp.doorZ;
        glm::vec3 outDir{0.0f};
        if (doorSide == South)      { oz -= 1; outDir = {0, 0, -1}; }
        else if (doorSide == North) { oz += 1; outDir = {0, 0, 1}; }
        else if (doorSide == West)  { ox -= 1; outDir = {-1, 0, 0}; }
        else                        { ox += 1; outDir = {1, 0, 0}; }
        const HeightField& hf = terrain;
        float outsideY = baseY;
        if (ox >= 0 && oz >= 0 && ox < hf.nodes && oz < hf.nodes)
            outsideY = hf.heights[hf.index(ox, oz)];
        const float drop = floorY - outsideY;
        if (drop > 0.45f) {
            const glm::vec2 doorC = g.center(fp.doorX, fp.doorZ);
            const glm::vec3 edge = glm::vec3{doorC.x, 0.0f, doorC.y} +
                                   outDir * (ts * 0.5f); // foundation slab edge
            const int nSteps = static_cast<int>(std::ceil(drop / 0.45f));
            const float rise = drop / nSteps;
            const float stepDepth = 0.7f;
            const float stepW = p.doorW + 0.4f;
            for (int k = 1; k < nSteps; ++k) {
                const float topY = floorY - rise * k;
                const glm::vec3 c = edge + outDir * (stepDepth * (k - 0.5f));
                const glm::vec3 half = (outDir.x != 0.0f)
                                           ? glm::vec3(stepDepth * 0.5f, 0.0f, stepW * 0.5f)
                                           : glm::vec3(stepW * 0.5f, 0.0f, stepDepth * 0.5f);
                const glm::vec3 smn{c.x - half.x, outsideY - 0.3f, c.z - half.z};
                addBox(md, boxes, smn,
                       {half.x * 2.0f, topY - smn.y, half.z * 2.0f},
                       /*walkable=*/true);
            }
        }
    }

    bool roofHole = false;
    glm::vec2 roofHoleMn{0.0f}, roofHoleMx{0.0f};

    for (int fl = 0; fl < floors; ++fl) {
        const float inset = p.insetPerFloor * fl * ts;
        const float y = floorY + fl * p.floorHeight;
        const float rw = bw - inset * 2.0f;
        const float rd = bd - inset * 2.0f;
        if (rw < ts || rd < ts) break;
        const glm::vec2 corner{mnT.x + inset, mnT.y + inset};

        // Wall height; ruins lose height per segment via chaos below.
        float wallH = p.floorHeight;
        const bool ruined = p.ruinChaos > 0.0f;

        struct Run {
            glm::vec3 start;
            glm::vec3 dir;
            float len;
            Side side;
        };
        const Run runs[4] = {
            {{corner.x, y, corner.y}, {1, 0, 0}, rw, South},
            {{corner.x, y, corner.y + rd}, {1, 0, 0}, rw, North},
            {{corner.x, y, corner.y}, {0, 0, 1}, rd, West},
            {{corner.x + rw, y, corner.y}, {0, 0, 1}, rd, East},
        };

        for (const Run& r : runs) {
            if (p.colonnade && fl == floors - 1 && r.side != doorSide) {
                // Temple: the upper register is pillars, not wall.
                const int count = std::max(2, static_cast<int>(r.len / 2.2f));
                for (int k = 0; k <= count; ++k) {
                    const float t = r.len * k / count;
                    const glm::vec3 at = r.start + r.dir * t;
                    addBox(md, boxes,
                           {at.x - 0.25f, y, at.z - 0.25f}, {0.5f, wallH, 0.5f});
                }
                continue;
            }
            std::vector<Opening> open;
            // Door slot, ground floor, on its side, at the validator's tile.
            if (fl == 0 && r.side == doorSide) {
                const float doorH = std::min(p.doorH, wallH - 0.2f);
                open.push_back({doorAlong, p.doorW, 0.0f, doorH});
                // A swinging panel for the game to hang in the hole. Ruins
                // get none — their doorway is just a gap in what's left.
                if (p.ruinChaos <= 0.0f) {
                    DoorSpec door;
                    door.hinge = r.start + r.dir * (doorAlong - p.doorW * 0.5f);
                    door.width = p.doorW;
                    door.height = doorH;
                    door.yawClosed = std::atan2(-r.dir.z, r.dir.x);
                    out.doors.push_back(door);
                }
            }
            // Window rhythm; never on top of the door slot.
            const int wins = static_cast<int>(r.len / p.windowEvery);
            for (int k = 1; k <= wins; ++k) {
                const float c = r.len * k / (wins + 1);
                if (fl == 0 && r.side == doorSide &&
                    std::fabs(c - doorAlong) < (p.doorW + p.windowW) * 0.7f)
                    continue;
                if (ruined && rng.next01() < p.ruinChaos * 0.5f) continue;
                open.push_back({c, p.windowW, p.windowSill,
                                std::min(p.windowSill + p.windowH, wallH - 0.15f)});
            }
            if (ruined) {
                // Crumble: chance to lose the run's top half, or the run
                // entirely (one wall of a ruin simply is not there anymore).
                const float roll = rng.next01();
                if (roll < p.ruinChaos * 0.25f) continue;
                if (roll < p.ruinChaos * 0.7f) wallH *= 0.4f + 0.4f * rng.next01();
            }
            addWallRun(md, boxes, r.start, r.dir, r.len, wallH,
                       p.wallThickness, std::move(open));
        }

        // Interior stair run hugging the wall opposite the door, whenever
        // there is somewhere to arrive: the next floor, or a flat roof above
        // the top register. Walkable full-height step boxes (each rise well
        // inside the player's 0.5 step-up), with the slab above cut open
        // over the strip so the climb actually lands. On insetting towers
        // the strip is placed against the NEXT floor's smaller rect, so the
        // run tops out inside the upper room, not on the ledge outside it.
        bool stairHole = false;
        glm::vec2 holeMn{0.0f}, holeMx{0.0f};
        const bool roofAccess = p.roof == "flat" && !p.colonnade && !ruined;
        const bool wantStair =
            p.stairs && !ruined &&
            (fl + 1 < floors || (fl == floors - 1 && roofAccess));
        if (wantStair) {
            const float total =
                (fl + 1 < floors) ? wallH : wallH + 0.25f; // up into the roof slab
            const float insetN = (fl + 1 < floors)
                                     ? p.insetPerFloor * (fl + 1) * ts
                                     : inset;
            const glm::vec2 sc{mnT.x + insetN, mnT.y + insetN};
            const float sw = bw - insetN * 2.0f;
            const float sd = bd - insetN * 2.0f;
            const float stripW = 1.1f;
            const float margin = p.wallThickness * 0.5f + 0.05f;
            const float endPad = 0.45f;
            glm::vec3 s0;     // run start, wall-side strip corner at floor y
            glm::vec3 runDir; // +x or +z
            float runLen;
            if (doorSide == South) {        // stair along the north wall
                s0 = {sc.x + endPad, y, sc.y + sd - margin - stripW};
                runDir = {1, 0, 0};
                runLen = sw - endPad * 2.0f;
            } else if (doorSide == North) { // along the south wall
                s0 = {sc.x + endPad, y, sc.y + margin};
                runDir = {1, 0, 0};
                runLen = sw - endPad * 2.0f;
            } else if (doorSide == West) {  // along the east wall
                s0 = {sc.x + sw - margin - stripW, y, sc.y + endPad};
                runDir = {0, 0, 1};
                runLen = sd - endPad * 2.0f;
            } else {                        // East door -> west wall
                s0 = {sc.x + margin, y, sc.y + endPad};
                runDir = {0, 0, 1};
                runLen = sd - endPad * 2.0f;
            }
            const int nSteps =
                std::max(4, static_cast<int>(std::ceil(total / 0.38f)));
            if (runLen / nSteps >= 0.24f && sw > stripW * 2.0f &&
                sd > stripW * 2.0f) {
                const float stepRun = runLen / nSteps;
                const float rise = total / nSteps;
                const glm::vec3 across = (runDir.x != 0.0f)
                                             ? glm::vec3(0, 0, stripW)
                                             : glm::vec3(stripW, 0, 0);
                for (int k = 0; k < nSteps; ++k) {
                    // Full column from the floor: the under-stair wedge is
                    // solid, keeping each step an honest AABB.
                    const glm::vec3 smn = s0 + runDir * (stepRun * k);
                    const glm::vec3 size = runDir * stepRun + across +
                                           glm::vec3(0, rise * (k + 1), 0);
                    addBox(md, boxes, glm::min(smn, smn + size),
                           glm::abs(size), /*walkable=*/true);
                }
                // The slab above opens over the whole strip (plus headroom
                // margin) so the climb never clips a ceiling.
                const glm::vec3 e1 = s0 + runDir * runLen + across;
                holeMn = glm::min(glm::vec2(s0.x, s0.z), glm::vec2(e1.x, e1.z)) -
                         glm::vec2(0.15f);
                holeMx = glm::max(glm::vec2(s0.x, s0.z), glm::vec2(e1.x, e1.z)) +
                         glm::vec2(0.15f);
                stairHole = true;
                if (fl == floors - 1) {
                    roofHole = true;
                    roofHoleMn = holeMn;
                    roofHoleMx = holeMx;
                }
            }
        }

        // Inter-floor slab (the upper floor's walkable floor), cut open over
        // the stairwell so the run arrives instead of bonking.
        if (fl + 1 < floors && !ruined) {
            addSlabWithHole(md, boxes, corner, rw, rd, y + wallH - 0.18f,
                            0.18f, stairHole, holeMn, holeMx);
        }

        // Interior lamp, sometimes: a floor-level anchor near the door wall
        // for the game to dress (pillar mesh, pulse, hum).
        if (rng.next01() < p.lampChance && rw > 2.0f && rd > 2.0f) {
            const float ax = corner.x + 0.8f + (rw - 1.6f) * rng.next01();
            const float az = corner.y + 0.8f + (rd - 1.6f) * rng.next01();
            glm::vec3 lp{ax, y, corner.y + 0.8f};
            if (doorSide == North) lp = {ax, y, corner.y + rd - 0.8f};
            if (doorSide == West)  lp = {corner.x + 0.8f, y, az};
            if (doorSide == East)  lp = {corner.x + rw - 0.8f, y, az};
            out.lamps.push_back(lp);
        }

        // A wall scrawl on the ground floor, sometimes: eye height on one of
        // the side walls (never the door wall), facing into the room. The
        // game keeps it invisible until the player's gaze finds it.
        if (fl == 0 && !ruined && rng.next01() < 0.55f) {
            WallMark mark;
            const float eyeY = y + 1.45f;
            const bool firstWall = (rng.next() & 1u) != 0;
            const float innerOff = p.wallThickness * 0.5f + 0.02f;
            if (doorSide == South || doorSide == North) {
                const float mz = corner.y + rd * (0.3f + 0.4f * rng.next01());
                mark.pos = firstWall
                               ? glm::vec3{corner.x + innerOff, eyeY, mz}
                               : glm::vec3{corner.x + rw - innerOff, eyeY, mz};
                mark.normal = firstWall ? glm::vec3{1, 0, 0} : glm::vec3{-1, 0, 0};
            } else {
                const float mx = corner.x + rw * (0.3f + 0.4f * rng.next01());
                mark.pos = firstWall
                               ? glm::vec3{mx, eyeY, corner.y + innerOff}
                               : glm::vec3{mx, eyeY, corner.y + rd - innerOff};
                mark.normal = firstWall ? glm::vec3{0, 0, 1} : glm::vec3{0, 0, -1};
            }
            out.scrawls.push_back(mark);
        }
    }

    // Roof over the top register.
    if (p.roof != "none" && p.ruinChaos <= 0.0f) {
        const float inset = p.insetPerFloor * (floors - 1) * ts;
        const float topY = floorY + floors * p.floorHeight;
        const glm::vec3 rmn{mnT.x + inset - 0.3f, topY, mnT.y + inset - 0.3f};
        const float rw = bw - inset * 2.0f + 0.6f;
        const float rd = bd - inset * 2.0f + 0.6f;
        if (p.roof == "gable") {
            addGable(md, boxes, rmn, rw, rd, std::min(rw, rd) * 0.45f);
        } else if (p.roof == "pyramid") {
            addPyramidRoof(md, boxes, rmn, rw, rd, std::min(rw, rd) * 0.5f);
        } else { // flat — walkable, with the stairwell cut when a run arrives
            addSlabWithHole(md, boxes, {rmn.x, rmn.z}, rw, rd, topY, 0.25f,
                            roofHole, roofHoleMn, roofHoleMx);
        }
    }

    out.mesh = std::move(md);
    out.boxes = std::move(boxes);
    out.zone = fp.zone;
    out.anchor = {g.center(fp.doorX, fp.doorZ).x, floorY,
                  g.center(fp.doorX, fp.doorZ).y};
    return out;
}

// --- bridge / pier runs --------------------------------------------------------

std::vector<DeckRun> collectRuns(const TileGrid& g, const TileSet& ts) {
    const int deckId = ts.roleId("deck");
    const int pierId = ts.roleId("pier");
    auto deckish = [&](int t) { return t == deckId || t == pierId; };
    const int n = g.n;
    std::vector<std::uint8_t> seen(static_cast<size_t>(n) * n, 0);
    std::vector<DeckRun> runs;
    constexpr int DX[4] = {-1, 1, 0, 0};
    constexpr int DZ[4] = {0, 0, -1, 1};

    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const size_t i = static_cast<size_t>(z) * n + x;
            const int t = static_cast<int>(g.at(x, z));
            if (seen[i] || !deckish(t)) continue;

            // Flood the component.
            std::vector<int> comp;
            std::deque<int> q{static_cast<int>(i)};
            seen[i] = 1;
            bool pier = (t == pierId);
            while (!q.empty()) {
                const int c = q.front();
                q.pop_front();
                comp.push_back(c);
                const int cx = c % n, cz = c / n;
                for (int k = 0; k < 4; ++k) {
                    const int nx = cx + DX[k], nz = cz + DZ[k];
                    if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
                    const size_t j = static_cast<size_t>(nz) * n + nx;
                    const int nt = static_cast<int>(g.at(nx, nz));
                    if (seen[j] || !deckish(nt)) continue;
                    if (nt == pierId) pier = true;
                    seen[j] = 1;
                    q.push_back(static_cast<int>(j));
                }
            }

            // Order into a chain: start from a degree-1 cell (an end) and
            // greedily walk unvisited neighbors. Branchy components walk in
            // some order; the slabs still cover every cell.
            auto degree = [&](int c) {
                const int cx = c % n, cz = c / n;
                int deg = 0;
                for (int k = 0; k < 4; ++k) {
                    const int nx = cx + DX[k], nz = cz + DZ[k];
                    if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
                    const int nt = static_cast<int>(g.at(nx, nz));
                    if (deckish(nt)) ++deg;
                }
                return deg;
            };
            int start = comp[0];
            for (int c : comp) {
                if (degree(c) <= 1) {
                    start = c;
                    break;
                }
            }
            std::vector<std::uint8_t> inChain(static_cast<size_t>(n) * n, 0);
            DeckRun run;
            run.isPier = pier;
            int cur = start;
            inChain[cur] = 1;
            run.cells.push_back(cur);
            for (;;) {
                const int cx = cur % n, cz = cur / n;
                int next = -1;
                for (int k = 0; k < 4; ++k) {
                    const int nx = cx + DX[k], nz = cz + DZ[k];
                    if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
                    const int j = nz * n + nx;
                    const int nt = static_cast<int>(g.at(nx, nz));
                    if (deckish(nt) && !inChain[j]) {
                        next = j;
                        break;
                    }
                }
                if (next < 0) break;
                inChain[next] = 1;
                run.cells.push_back(next);
                cur = next;
            }
            // Cells missed by the chain walk (branches) are appended AFTER the
            // chain length is recorded, so the end-height interpolation never
            // mistakes a mid-span branch cell for the far end of the run.
            run.chainLen = static_cast<int>(run.cells.size());
            for (int c : comp)
                if (!inChain[c]) run.cells.push_back(c);
            runs.push_back(std::move(run));
        }
    }
    return runs;
}

BuiltPiece expandDeck(const TileGrid& g, const HeightField& hf,
                      const TileSet& tileset, const DeckRun& run) {
    const int deckId = tileset.roleId("deck");
    const int pierId = tileset.roleId("pier");
    const int n = g.n;
    BuiltPiece out;
    MeshData md;
    std::vector<PartBox> boxes;

    // End heights: walkable land next to the chain's first/last cell
    // (fallback: the water line — a stump, which corruption produces on
    // purpose). The deck linearly interpolates between them, floored so it
    // always clears the water. Branch cells appended past chainLen take the
    // nearest chain cell's height instead of skewing the interpolation.
    const int chainLen = run.chainLen > 0
                             ? run.chainLen
                             : static_cast<int>(run.cells.size());
    struct Shore {
        bool found = false;
        int x = 0, z = 0; // the land tile
        float y = 0.0f;
    };
    auto landYNear = [&](int cell) {
        const int cx = cell % n, cz = cell / n;
        constexpr int DX[4] = {-1, 1, 0, 0};
        constexpr int DZ[4] = {0, 0, -1, 1};
        Shore s;
        for (int k = 0; k < 4; ++k) {
            const int nx = cx + DX[k], nz = cz + DZ[k];
            if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
            const int nt = static_cast<int>(g.at(nx, nz));
            if (tileset.walkable(nt) && nt != deckId && nt != pierId) {
                s.found = true;
                s.x = nx;
                s.z = nz;
                s.y = hf.tileHeight(nx, nz);
                return s;
            }
        }
        s.y = hf.hasWater ? hf.waterLevel : hf.tileHeight(cx, cz);
        return s;
    };
    const Shore shore0 = landYNear(run.cells.front());
    const Shore shore1 = landYNear(run.cells[chainLen - 1]);
    const float y0 = shore0.y;
    const float y1 = shore1.y;
    const float minY = hf.hasWater ? hf.waterLevel + 0.55f : -1000.0f;

    const float ts = g.tileSize;
    const float deckThick = 0.3f;
    std::vector<float> chainTop(static_cast<size_t>(chainLen), 0.0f);
    for (int i = 0; i < chainLen; ++i) {
        const float t =
            chainLen > 1 ? static_cast<float>(i) / (chainLen - 1) : 0.0f;
        chainTop[i] = std::max(y0 + (y1 - y0) * t, minY);
    }
    for (size_t i = 0; i < run.cells.size(); ++i) {
        const int cell = run.cells[i];
        const int cx = cell % n, cz = cell / n;
        float topY;
        if (i < static_cast<size_t>(chainLen)) {
            topY = chainTop[i];
        } else {
            // Branch cell: ride at the height of the nearest chain cell.
            int best = 0, bestD = 1 << 30;
            for (int j = 0; j < chainLen; ++j) {
                const int jx = run.cells[j] % n, jz = run.cells[j] / n;
                const int dist = std::abs(jx - cx) + std::abs(jz - cz);
                if (dist < bestD) {
                    bestD = dist;
                    best = j;
                }
            }
            topY = chainTop[best];
        }
        const glm::vec2 c = g.center(cx, cz);
        addBox(md, boxes,
               {c.x - ts * 0.5f, topY - deckThick, c.y - ts * 0.5f},
               {ts, deckThick, ts}, /*walkable=*/true);
        // Posts every other slab — and always at both chain ends, so a run
        // never visibly hovers at its landing.
        if ((i % 2) == 0 || i == static_cast<size_t>(chainLen) - 1) {
            const float ground = hf.tileHeight(cx, cz);
            if (topY - deckThick - ground > 0.3f) {
                addBox(md, boxes,
                       {c.x - 0.18f, ground - 0.3f, c.y - 0.18f},
                       {0.36f, topY - deckThick - ground + 0.3f, 0.36f});
            }
        }
    }

    // Shore aprons: a short walkable lip from each anchored end onto its land
    // tile, so the deck meets the terrain instead of ending flush at the tile
    // seam with a gap. Stump ends (no land neighbor) stay bare on purpose.
    auto apron = [&](const Shore& s, int endCell, float endTopY) {
        if (!s.found) return;
        const int ex = endCell % n, ez = endCell / n;
        const glm::vec2 ec = g.center(ex, ez);
        const glm::vec2 lc = g.center(s.x, s.z);
        const glm::vec2 dir = glm::normalize(lc - ec); // axis-aligned step
        const float depth = ts * 0.35f;
        const float width = ts * 0.9f;
        // From the deck slab edge, reaching onto the shore tile.
        const glm::vec2 from = ec + dir * (ts * 0.5f);
        const glm::vec2 to = from + dir * depth;
        const glm::vec2 perp{-dir.y, dir.x};
        const glm::vec2 a = from - perp * (width * 0.5f);
        const glm::vec2 b = to + perp * (width * 0.5f);
        const glm::vec2 mn2 = glm::min(a, b), mx2 = glm::max(a, b);
        const float bottom = std::min(s.y, endTopY) - 0.35f;
        addBox(md, boxes, {mn2.x, bottom, mn2.y},
               {mx2.x - mn2.x, endTopY - bottom, mx2.y - mn2.y},
               /*walkable=*/true);
    };
    apron(shore0, run.cells.front(), chainTop.front());
    apron(shore1, run.cells[chainLen - 1], chainTop.back());
    out.mesh = std::move(md);
    out.boxes = std::move(boxes);
    if (!run.cells.empty()) {
        const int cell = run.cells.front();
        const glm::vec2 c = g.center(cell % n, cell / n);
        out.anchor = {c.x, std::max(y0, minY), c.y};
    }
    return out;
}

} // namespace liminal::procgen
