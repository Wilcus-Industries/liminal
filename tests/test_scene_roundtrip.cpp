// .lscene round-trip test. Pure CPU — no window, no GL: author a scene with
// every built-in component, save it, load it back, and require (a) the JSON
// bodies match exactly and (b) a second save is byte-identical to the first
// (save/load/save stability, as serialize.hpp promises).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <liminal/scene/components.hpp>
#include <liminal/scene/scene.hpp>
#include <liminal/scene/serialize.hpp>

namespace {

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

int main() {
    namespace fs = std::filesystem;

    liminal::Scene scene;

    liminal::Entity crate = scene.create("crate");
    crate.add<liminal::Transform>({.position = {1.0f, 2.5f, -3.0f},
                                   .rotationEuler = {0.0f, 45.0f, 0.0f},
                                   .scale = {1.0f, 2.0f, 1.0f}});
    crate.add<liminal::MeshRenderer>(
        {.meshAsset = "builtin:box",
         .color = {0.8f, 0.6f, 0.4f, 1.0f},
         .textureAsset = "builtin:checker"});

    liminal::Entity cam = scene.create("camera");
    cam.add<liminal::Transform>({.position = {0.0f, 4.0f, 8.0f}});
    cam.add<liminal::Camera>({.fovDeg = 65.0f, .primary = true});

    liminal::Entity mood = scene.create("mood");
    mood.add<liminal::Light>({.color = {1.0f, 0.9f, 0.7f}, .intensity = 0.8f});
    mood.add<liminal::AudioSource>({.gain = 0.2f, .enabled = true});
    mood.add<liminal::Script>({.paths = {"scripts/spin.lua"}});

    const fs::path path =
        fs::temp_directory_path() / "liminal_test_roundtrip.lscene";

    if (!scene.save(path.string())) {
        std::fprintf(stderr, "FAIL: save() returned false (%s)\n",
                     path.string().c_str());
        return 1;
    }
    const std::string firstBody = slurp(path);

    liminal::Scene loaded = liminal::Scene::load(path.string());

    if (loaded.entityCount() != scene.entityCount()) {
        std::fprintf(stderr, "FAIL: entity count %zu != %zu after load\n",
                     loaded.entityCount(), scene.entityCount());
        return 1;
    }
    if (liminal::sceneToJson(loaded) != liminal::sceneToJson(scene)) {
        std::fprintf(stderr, "FAIL: loaded scene JSON differs from source\n");
        return 1;
    }

    if (!loaded.save(path.string())) {
        std::fprintf(stderr, "FAIL: second save() returned false\n");
        return 1;
    }
    if (slurp(path) != firstBody) {
        std::fprintf(stderr, "FAIL: save/load/save is not byte-stable\n");
        return 1;
    }

    fs::remove(path);
    std::printf("OK: %zu entities round-tripped, save/load/save byte-stable\n",
                scene.entityCount());
    return 0;
}
