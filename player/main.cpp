// liminal-player: a standalone game runtime. The game's project files (scenes,
// scripts, textures, project.ljson) live in a pak appended to this executable
// or in a sidecar "<exe>.pak" beside it. We mount that pak as the asset VFS,
// read project.ljson for the title + startup scene, then hand off to App's
// canonical run loop. Hot reload is disabled (no source tree to watch). ESC or
// the window close button quits. Cross-platform: only std::filesystem + the
// pak's own binary I/O; no POSIX-only calls here.
#include <liminal/core/app.hpp>
#include <liminal/core/assets.hpp>
#include <liminal/core/pak.hpp>
#include <liminal/core/platform.hpp>
#include <liminal/render/renderer.hpp>
#include <liminal/scene/scene.hpp>

#include <nlohmann/json.hpp>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Register the shipped game's custom shaders into the renderer, mirroring the
// editor's on-disk discovery (EditorApp::scanShaders) but reading through the
// mounted pak VFS instead of the disk. The pak's index is enumerable
// (PakReader::paths), so we walk every "shaders/..." entry:
//   - shaders/<name>/scene.vert + shaders/<name>/scene.frag  -> full pack
//   - shaders/<name>.frag                                    -> frag-only pack
// The built-ins "native"/"retro" are always registered by the Renderer ctor;
// this only adds creator shaders so a scene's Camera.shaderName can resolve.
// Missing shaders/ entirely is a no-op (games with no custom shaders).
// Registration is lazy-compile (the pack source is stored; it compiles on the
// first useShaderPack switch during rendering), so a GL context is not yet
// strictly required here, but App has already created one regardless.
void registerPakShaders(liminal::Renderer& renderer,
                        const liminal::PakReader& pak) {
    auto& catalog = liminal::shaderCatalog();
    // Discovered names, deduped and sorted for a stable catalog order. Maps the
    // shader name -> whether it's a full pack (true) or frag-only (false).
    std::map<std::string, bool> full;

    for (const auto& key : pak.paths()) {
        // Keys are pak-normalized ('/'-sep, no leading "./"). Match shaders/*.
        constexpr const char* kPrefix = "shaders/";
        if (key.rfind(kPrefix, 0) != 0) continue;
        const std::string rest = key.substr(std::string(kPrefix).size());
        if (rest.empty()) continue;

        const auto slash = rest.find('/');
        if (slash != std::string::npos) {
            // shaders/<name>/scene.frag -> full pack candidate. We key off the
            // frag and pair it with the sibling vert at read time.
            const std::string sub = rest.substr(slash + 1);
            if (sub == "scene.frag") full[rest.substr(0, slash)] = true;
        } else if (rest.size() > 5 &&
                   rest.compare(rest.size() - 5, 5, ".frag") == 0) {
            // shaders/<name>.frag -> frag-only. (A bare .vert with no sibling
            // .frag is not a usable pack, so we ignore lone verts.)
            full[rest.substr(0, rest.size() - 5)] = false;
        }
    }

    for (const auto& [name, isFull] : full) {
        try {
            liminal::ShaderPack pack;
            if (isFull) {
                const auto vert =
                    liminal::Assets::readFile("shaders/" + name + "/scene.vert");
                const auto frag =
                    liminal::Assets::readFile("shaders/" + name + "/scene.frag");
                if (!vert || !frag) {
                    std::fprintf(stderr,
                                 "liminal-player: shader '%s' missing scene "
                                 "vert/frag in pak; skipping\n",
                                 name.c_str());
                    continue;
                }
                pack = liminal::ShaderPack::makeFullPack(*vert, *frag);
            } else {
                const auto frag =
                    liminal::Assets::readFile("shaders/" + name + ".frag");
                if (!frag) continue;
                pack = liminal::ShaderPack::makeFragOnlyPack(*frag);
            }
            pack.label = name;
            renderer.registerShaderPack(name, std::move(pack));
            if (std::find(catalog.begin(), catalog.end(), name) ==
                catalog.end())
                catalog.push_back(name);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "liminal-player: skip shader '%s': %s\n",
                         name.c_str(), e.what());
        }
    }
}

} // namespace

int main() {
    const std::string exe = liminal::selfExePath();
    const std::string dir = liminal::selfExeDir();
    if (exe.empty() || dir.empty()) {
        std::fprintf(stderr, "liminal-player: cannot locate own executable\n");
        return 1;
    }

    // Try the pak appended to this binary first; fall back to a sidecar named
    // "<exeStem>.pak" beside it. The stem drops a Windows ".exe" automatically.
    auto pak = std::make_shared<liminal::PakReader>();
    if (!pak->open(exe)) {
        const fs::path sidecar =
            fs::path(dir) / (fs::path(exe).stem().string() + ".pak");
        if (!pak->open(sidecar.generic_string())) {
            std::fprintf(stderr,
                         "liminal-player: no game data (no pak appended to the "
                         "binary and no '%s' beside it)\n",
                         sidecar.generic_string().c_str());
            return 1;
        }
    }

    liminal::Assets::mountPak(pak);
    // Models / external assets (e.g. .gguf) ship beside the exe, not in the pak.
    liminal::Assets::addSearchPath(dir);

    // project.ljson is pak-local (the build step writes one with assetRoot ".",
    // so paths inside resolve directly through the mounted pak).
    const auto projTxt = liminal::Assets::readFile("project.ljson");
    if (!projTxt) {
        std::fprintf(stderr, "liminal-player: game data has no project.ljson\n");
        return 1;
    }

    std::string title = "liminal";
    std::string startup;
    int width = 1280, height = 720;
    try {
        const nlohmann::json j = nlohmann::json::parse(*projTxt);
        title = j.value("title", std::string("liminal"));
        startup = j.value("startupScene", std::string());
        width = j.value("width", width);
        height = j.value("height", height);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "liminal-player: bad project.ljson: %s\n", e.what());
        return 1;
    }

    liminal::AppConfig cfg;
    cfg.title = title;
    cfg.width = width;
    cfg.height = height;
    cfg.hotReload = false; // shipped game: no source tree to watch

    liminal::App app(cfg);

    // The Renderer ctor always registers the built-ins "native"/"retro".
    // Seed the shared catalog with them, then discover the game's own shaders
    // from the mounted pak so a scene's Camera.shaderName resolves at runtime.
    liminal::shaderCatalog() = {"native", "retro"};
    registerPakShaders(app.renderer(), *pak);

    if (!startup.empty()) {
        // Scene::load routes through Assets::readFile, so it pulls from the pak.
        // A bad/missing scene must not crash the player — log and run empty.
        try {
            app.scene() = liminal::Scene::load(startup);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "liminal-player: failed to load startup scene '%s': "
                         "%s\n",
                         startup.c_str(), e.what());
        }
    } else {
        std::fprintf(stderr,
                     "liminal-player: project.ljson has no startupScene; "
                     "running an empty scene\n");
    }

    app.run([](liminal::Frame& f) {
        if (f.input.keyPressed(GLFW_KEY_ESCAPE)) f.app.quit();
    });
    return 0;
}
