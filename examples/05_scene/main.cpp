// 05_scene: the Scene/App showcase. Builds a small scene in code (boxes, a
// pillar, a camera), saves it to .lscene, loads it back, asserts the
// round-trip is byte-identical JSON, then renders the LOADED scene through
// App's built-in render system. ESC quits.
#include <liminal/core/app.hpp>
#include <liminal/scene/scene.hpp>
#include <liminal/scene/serialize.hpp>

#include <GLFW/glfw3.h>

#include <cassert>
#include <cstdio>
#include <utility>

namespace {

void buildScene(liminal::Scene& scene) {
    liminal::Entity cam = scene.create("camera");
    cam.add<liminal::Transform>({.position = {0.0f, 2.2f, 6.0f},
                                 .rotationEuler = {-12.0f, 0.0f, 0.0f}});
    cam.add<liminal::Camera>({.fovDeg = 70.0f});

    const struct {
        const char* name;
        glm::vec3 pos;
        glm::vec4 color;
        const char* tex;
    } boxes[] = {
        {"crate_a", {-1.6f, 0.0f, 0.0f}, {0.65f, 0.50f, 0.75f, 1.0f}, "builtin:wood"},
        {"crate_b", {0.0f, 0.0f, -0.8f}, {0.55f, 0.70f, 0.60f, 1.0f}, "builtin:brick"},
        {"crate_c", {1.6f, 0.0f, 0.2f}, {0.80f, 0.72f, 0.50f, 1.0f}, "builtin:checker"},
    };
    for (const auto& b : boxes) {
        liminal::Entity e = scene.create(b.name);
        e.add<liminal::Transform>({.position = b.pos});
        e.add<liminal::MeshRenderer>(
            {.meshAsset = "builtin:box", .color = b.color, .textureAsset = b.tex});
    }

    liminal::Entity pillar = scene.create("pillar");
    pillar.add<liminal::Transform>({.position = {0.0f, 0.0f, -3.0f}});
    pillar.add<liminal::MeshRenderer>({.meshAsset = "builtin:pillar",
                                       .color = {0.6f, 0.6f, 0.7f, 1.0f},
                                       .textureAsset = "builtin:concrete"});

    liminal::Entity ground = scene.create("ground");
    ground.add<liminal::Transform>({.position = {0.0f, -0.05f, 0.0f},
                                    .scale = {14.0f, 1.0f, 14.0f}});
    ground.add<liminal::MeshRenderer>({.meshAsset = "builtin:plane",
                                       .color = {0.45f, 0.5f, 0.42f, 1.0f},
                                       .textureAsset = "builtin:grass"});
}

} // namespace

int main() {
    const char* path = "05_scene.lscene";

    // Author, save, reload, verify — all before the window exists (the scene
    // layer is GPU-free; only the App's render system touches GL).
    liminal::Scene authored;
    buildScene(authored);
    if (!authored.save(path)) return 1;

    liminal::Scene loaded = liminal::Scene::load(path);
    assert(loaded.entityCount() == authored.entityCount());
    assert(loaded.find("crate_b").get<liminal::Transform>().position ==
           authored.find("crate_b").get<liminal::Transform>().position);
    assert(loaded.find("camera").get<liminal::Camera>().primary);

    // Round-trip gate: save -> load -> save must produce identical JSON.
    const nlohmann::json a = liminal::sceneToJson(authored);
    const nlohmann::json b = liminal::sceneToJson(loaded);
    if (a != b) {
        std::fprintf(stderr, "05_scene: round-trip JSON mismatch!\n");
        return 1;
    }
    std::printf("05_scene: round-trip OK (%zu entities)\n", loaded.entityCount());

    liminal::App app({.title = "liminal — 05_scene", .width = 1280, .height = 720,
                      .audio = false});
    app.scene() = std::move(loaded); // render the LOADED scene, not the authored one

    app.run([&](liminal::Frame& f) {
        if (f.input.keyPressed(GLFW_KEY_ESCAPE)) f.app.quit();
        // Idle spin on the crates proves each<> mutation is live.
        f.scene.each<liminal::Transform, liminal::MeshRenderer>(
            [&](liminal::Entity e, liminal::Transform& t, liminal::MeshRenderer&) {
                if (e.has<liminal::Name>() &&
                    e.get<liminal::Name>().value.rfind("crate", 0) == 0) {
                    t.rotationEuler.y += 40.0f * f.dt;
                }
            });
    });
    return 0;
}
