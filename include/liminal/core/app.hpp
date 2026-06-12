#pragma once
// App: the batteries-included bootstrap. Owns Window + Renderer + Audio +
// ImGuiLayer + Scene + AssetCache and runs the canonical loop so a complete
// program is:
//
//   liminal::App app({.title = "demo", .width = 1280, .height = 720});
//   liminal::Entity box = app.scene().create("box");
//   box.add<liminal::Transform>({.position = {0, 0, -4}});
//   box.add<liminal::MeshRenderer>({.meshAsset = "builtin:box"});
//   app.run([&](liminal::Frame& f) {
//       if (f.input.keyPressed(GLFW_KEY_ESCAPE)) f.app.quit();
//   });
//
// Per frame, run():
//   1. polls events, computes dt (clamped to 0.1s so debugger pauses don't
//      catapult the simulation), begins the ImGui frame
//   2. applies the first enabled AudioSource to Audio::params
//   3. renderer.beginFrame(view from the primary Camera entity, or a default
//      eye at (0,2,5) looking at the origin if no camera exists)
//   4. calls the user fn — free to submit extra DrawItems / draw ImGui panels
//   5. built-in render system: each<Transform, MeshRenderer> -> DrawItem
//      (mesh/texture resolved by name through AssetCache; entities whose mesh
//      isn't resolvable are skipped), then endFrame + ImGui + swap
//
// App is a convenience layer: everything it owns is reachable by reference,
// and engine modules remain fully usable without it (Grey Matter never sees
// this class).

#include <functional>
#include <memory>
#include <string>

#include <liminal/core/asset_cache.hpp>
#include <liminal/core/window.hpp>
#include <liminal/render/renderer.hpp>
#include <liminal/render/types.hpp>
#include <liminal/scene/scene.hpp>

namespace liminal {

class Audio;
class ImGuiLayer;
class App;
#if defined(LIMINAL_WITH_SCRIPTING)
class ScriptHost;
#endif

struct AppConfig {
    std::string title = "liminal";
    int width = 1280;
    int height = 720;
    RenderSettings render{};
    bool vsync = true;
    bool audio = true;          // Audio device is optional (CI/headless)
    unsigned int audioSeed = 1;
};

// Everything a frame callback needs, by reference. Valid only for the
// duration of the callback.
struct Frame {
    float dt;          // seconds since last frame, clamped to 0.1
    double time;       // seconds since App construction
    Window& input;     // keyboard/mouse (it's the Window; alias for intent)
    Scene& scene;
    Renderer& renderer;
    AssetCache& assets;
    Audio* audio;      // nullptr when AppConfig.audio == false or device failed
    App& app;
};

class App {
public:
    explicit App(const AppConfig& config = {});
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Runs the loop until the window closes or quit() is called. The callback
    // may be empty (nullptr) — the built-in scene render still happens.
    void run(const std::function<void(Frame&)>& frameFn);
    void quit();

    Window& window() { return *m_window; }
    Renderer& renderer() { return *m_renderer; }
    Scene& scene() { return m_scene; }
    AssetCache& assets() { return m_assets; }
    Audio* audio() { return m_audio.get(); }       // may be null (see config)
    ImGuiLayer& imgui() { return *m_imgui; }
#if defined(LIMINAL_WITH_SCRIPTING)
    // Lua behavior for Script components; updated each frame between the
    // user callback and the built-in scene render.
    ScriptHost& scripts() { return *m_scripts; }
#endif

private:
    void renderScene(); // the built-in each<Transform, MeshRenderer> system
    glm::mat4 primaryCameraView(); // also pushes Camera.fovDeg into settings

    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Audio> m_audio;
    std::unique_ptr<ImGuiLayer> m_imgui;
#if defined(LIMINAL_WITH_SCRIPTING)
    std::unique_ptr<ScriptHost> m_scripts;
#endif
    Scene m_scene;
    AssetCache m_assets;
    double m_lastTime = 0.0;
};

} // namespace liminal
