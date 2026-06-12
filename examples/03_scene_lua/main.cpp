// 03_scene_lua: Lua scripting showcase. Boxes carry Script components —
// three run spin.lua (rotate + color pulse), two run bob.lua (sine-bob with
// per-entity phase, proving environment isolation: same file, independent
// state). The App's ScriptHost picks them up automatically; no script code
// in main. Edit scripts/spin.lua while this runs to see hot reload. ESC quits.
#include <liminal/core/app.hpp>
#include <liminal/core/assets.hpp>
#include <liminal/scene/scene.hpp>

#include <GLFW/glfw3.h>

int main() {
    // Make "scripts/spin.lua" resolvable (and hot-reload edits in the repo).
    liminal::Assets::addSearchPath(SCENE_LUA_DIR);

    liminal::App app({.title = "liminal — 03_scene_lua",
                      .width = 1280,
                      .height = 720,
                      // vsync off so an occluded window keeps ticking at full
                      // speed (macOS throttles occluded vsync'd swaps).
                      .vsync = false,
                      .audio = false});
    liminal::Scene& scene = app.scene();

    liminal::Entity cam = scene.create("camera");
    cam.add<liminal::Transform>({.position = {0.0f, 2.5f, 7.0f},
                                 .rotationEuler = {-14.0f, 0.0f, 0.0f}});
    cam.add<liminal::Camera>({.fovDeg = 70.0f});

    liminal::Entity ground = scene.create("ground");
    ground.add<liminal::Transform>({.position = {0.0f, -0.55f, 0.0f},
                                    .scale = {16.0f, 1.0f, 16.0f}});
    ground.add<liminal::MeshRenderer>({.meshAsset = "builtin:plane",
                                       .color = {0.40f, 0.44f, 0.40f, 1.0f},
                                       .textureAsset = "builtin:grid"});

    const struct {
        const char* name;
        glm::vec3 pos;
        const char* script;
    } actors[] = {
        {"spinner_a", {-2.4f, 0.0f, 0.0f}, "scripts/spin.lua"},
        {"spinner_b", {0.0f, 0.0f, -1.0f}, "scripts/spin.lua"},
        {"spinner_c", {2.4f, 0.0f, 0.3f}, "scripts/spin.lua"},
        {"bobber_a", {-1.2f, 0.0f, 2.0f}, "scripts/bob.lua"},
        {"bobber_b", {1.2f, 0.0f, 2.0f}, "scripts/bob.lua"},
    };
    for (const auto& a : actors) {
        liminal::Entity e = scene.create(a.name);
        e.add<liminal::Transform>({.position = a.pos});
        e.add<liminal::MeshRenderer>({.meshAsset = "builtin:box",
                                      .color = {0.7f, 0.7f, 0.8f, 1.0f},
                                      .textureAsset = "builtin:checker"});
        e.add<liminal::Script>({.path = a.script});
    }

    app.run([](liminal::Frame& f) {
        if (f.input.keyPressed(GLFW_KEY_ESCAPE)) f.app.quit();
    });
    return 0;
}
