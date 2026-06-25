// Headless regression cover for three new Lua/engine surfaces — no window, no
// GL context:
//   (2a) lm.json.encode / lm.json.decode round-trip (pure Lua),
//   (2b) lm.import shared-state caching (module table identity across imports),
//   (2c) raycastScene against a Collider AABB (C++ direct call, no Lua, no GL).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <glm/glm.hpp>

#include <liminal/core/asset_cache.hpp>
#include <liminal/core/assets.hpp>
#include <liminal/scene/components.hpp>
#include <liminal/scene/raycast.hpp>
#include <liminal/scene/scene.hpp>
#include <liminal/script/script_host.hpp>

namespace {

#define FAIL(msg)                                                              \
    do {                                                                       \
        std::fprintf(stderr, "FAIL: %s\n", (msg));                            \
        return 1;                                                              \
    } while (0)

// (2a) lm.json round-trip — encode a mixed table, decode it, verify fields.
int testJson() {
    liminal::ScriptHost host; // windowless
    sol::state& lua = host.lua();
    const char* src = R"LUA(
        local t = { a = 1, b = { 10, 20, 30 }, c = "hi", d = true }
        local s = lm.json.encode(t)
        local u = lm.json.decode(s)
        assert(u.a == 1, "a")
        assert(u.b[2] == 20, "b2")
        assert(u.c == "hi", "c")
        assert(u.d == true, "d")
        assert(#u.b == 3, "blen")
        return 1
    )LUA";
    sol::protected_function_result r =
        lua.safe_script(src, sol::script_pass_on_error);
    if (!r.valid()) {
        sol::error e = r;
        std::fprintf(stderr, "FAIL: lm.json lua error: %s\n", e.what());
        return 1;
    }
    if (r.get<double>() != 1.0) FAIL("lm.json round-trip did not return 1");
    return 0;
}

// (2b) lm.import shared-state — repeated imports of the same path return the
// same cached table (mutations are visible across imports).
int testImport() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "liminal_test_import";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "shared_mod.lua");
        f << "return { counter = 0 }\n";
    }
    liminal::Assets::addSearchPath(dir.string()); // so resolve() finds it

    int rc = 0;
    {
        liminal::ScriptHost host; // windowless
        sol::state& lua = host.lua();
        const char* src = R"LUA(
            local m1 = lm.import("shared_mod.lua")
            m1.counter = m1.counter + 7
            local m2 = lm.import("shared_mod.lua")   -- same cached table
            assert(m2.counter == 7, "import did not share state: " .. tostring(m2.counter))
            assert(m1 == m2, "import returned different tables")
            return m2.counter
        )LUA";
        sol::protected_function_result r =
            lua.safe_script(src, sol::script_pass_on_error);
        if (!r.valid()) {
            sol::error e = r;
            std::fprintf(stderr, "FAIL: lm.import lua error: %s\n", e.what());
            rc = 1;
        } else if (r.get<double>() != 7.0) {
            std::fprintf(stderr, "FAIL: lm.import counter != 7\n");
            rc = 1;
        }
    }

    std::error_code ec;
    fs::remove_all(dir, ec); // cleanup, non-fatal
    return rc;
}

// (2c) raycastScene against a Collider AABB — no GL (collider path skips mesh).
int testRaycast() {
    liminal::Scene scene;
    liminal::Entity box = scene.create("box");
    box.add<liminal::Transform>({.position = {0.0f, 0.0f, 0.0f}});
    box.add<liminal::Collider>(
        {.center = {0.0f, 0.0f, 0.0f}, .halfExtents = {1.0f, 1.0f, 1.0f}});

    liminal::AssetCache assets; // default ctor — no GL; mesh() never called

    // Ray from +Z looking toward -Z straight at the box.
    auto hit = liminal::raycastScene(scene, assets, glm::vec3(0.0f, 0.0f, 5.0f),
                                     glm::vec3(0.0f, 0.0f, -1.0f), 100.0f);
    if (!hit) FAIL("raycast missed a collider directly in front");
    if (hit->entity != box.handle()) FAIL("raycast hit the wrong entity");
    if (std::abs(hit->distance - 4.0f) > 1e-3f)
        FAIL("raycast distance wrong (expected ~4)");
    if (hit->normal.z <= 0.5f) FAIL("raycast normal not the +Z face");

    // A ray pointing AWAY from the box must miss.
    auto miss = liminal::raycastScene(scene, assets, glm::vec3(0, 0, 5),
                                      glm::vec3(0, 0, 1), 100.0f);
    if (miss) FAIL("raycast hit a box that is behind the ray");
    return 0;
}

// (2d) Mass create/destroy via a Script — mirrors the Maze project's
// on_start -> build_world -> clear_world -> rebuild flow that crashed the editor
// (SIGSEGV in Entity::valid from entity:destroy). Builds many entities (each
// taking a raw Transform/MeshRenderer pointer into Lua, growing the pools),
// collects them with lm.scene.each, destroys them through held Entity userdata,
// then rebuilds — twice, to mimic a hot-reload re-run against a populated scene.
// Runs headless (no GL): builtin meshes are never uploaded, only registry data
// is touched. Must complete with no crash / no ASan report and a clean scene.
int testMassDestroy() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "liminal_test_massdestroy";
    fs::create_directories(dir);
    const fs::path script = dir / "repro.lua";
    {
        std::ofstream f(script);
        f << R"LUA(
local function build(n)
    for i = 1, n do
        local e = lm.scene.create("wall_" .. i)
        e:add_component("MeshRenderer", { mesh = "builtin:box" })
        local t = e:get_transform()        -- raw Transform* into Lua
        t:set_position(i, 0, 0)
        t:set_scale(1, 2, 1)
        local mr = e:get_mesh_renderer()   -- raw MeshRenderer* into Lua
        if mr then mr:set_color(0.3, 0.3, 0.3) end
    end
end

local function clear()
    local kill = {}
    lm.scene.each(function(e)
        local nm = e.name or ""
        if nm:match("^wall_") then kill[#kill + 1] = e end
    end)
    for _, e in ipairs(kill) do e:destroy() end
end

local M = {}
function M.on_start(self)
    -- round 1: build then clear the populated scene
    build(200)
    clear()
    -- round 2: rebuild against the just-cleared scene, clear again
    build(200)
    clear()
end
function M.on_update(self, dt) end
return M
)LUA";
    }
    liminal::Assets::addSearchPath(dir.string());

    int rc = 0;
    {
        liminal::Scene scene;
        liminal::Entity runner = scene.create("runner");
        liminal::Script sc;
        sc.paths = {script.string()};
        runner.add<liminal::Script>(sc);

        liminal::ScriptHost host; // windowless, hotReload off
        // First update drives initInstance -> on_start (build/clear x2).
        host.update(scene, 0.016f);
        // Second update re-enters on_update only; the entity survives.
        host.update(scene, 0.016f);

        // Only the runner (+ its Name) should remain; all wall_* destroyed.
        std::size_t walls = 0;
        scene.each<liminal::Name>([&](liminal::Entity, liminal::Name& n) {
            if (n.value.rfind("wall_", 0) == 0) ++walls;
        });
        if (walls != 0) {
            std::fprintf(stderr,
                         "FAIL: mass-destroy left %zu wall entities alive\n",
                         walls);
            rc = 1;
        }
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    return rc;
}

} // namespace

int main() {
    if (testJson() != 0) return 1;
    if (testImport() != 0) return 1;
    if (testRaycast() != 0) return 1;
    if (testMassDestroy() != 0) return 1;
    std::printf("OK\n");
    return 0;
}
