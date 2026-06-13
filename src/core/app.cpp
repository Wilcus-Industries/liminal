#include <liminal/core/app.hpp>

#include <liminal/audio/audio.hpp>
#include <liminal/scene/component_registry.hpp>
#include <liminal/ui/imgui_layer.hpp>
#include <liminal/script/script_host.hpp>
#if defined(LIMINAL_WITH_INFERENCE)
#include <liminal/inference/engine.hpp>
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>
#include <exception>

namespace liminal {

App::App(const AppConfig& config) {
    m_window = std::make_unique<Window>(config.width, config.height, config.title);
    m_window->setVsync(config.vsync);
    m_renderer = std::make_unique<Renderer>();
    m_renderer->settings = config.render;
    if (config.audio) {
        auto audio = std::make_unique<Audio>(config.audioSeed);
        if (audio->ok()) m_audio = std::move(audio); // device failure -> no audio
    }
    m_imgui = std::make_unique<ImGuiLayer>(*m_window);
    registerBuiltinComponents();
    m_hotReload = config.hotReload;
    buildScriptHost();
    m_lastTime = m_window->time();
}

void App::buildScriptHost() {
    ScriptContext ctx;
    ctx.input = m_window.get();
    ctx.audio = m_audio.get();
    ctx.assets = &m_assets;
    ctx.hotReload = m_hotReload;
#if defined(LIMINAL_WITH_INFERENCE)
    // The engine ctor is cheap (no worker thread until lm.ai.start); create it
    // lazily on first host build and keep it across rebuilds so an in-flight
    // model survives a scene change.
    if (!m_inference) m_inference = std::make_unique<inference::Engine>();
    ctx.inference = m_inference.get();
#endif
    ctx.requestSceneChange = [this](const std::string& path) {
        m_pendingScene = path;
    };
    m_scripts = std::make_unique<ScriptHost>(std::move(ctx));
}

App::~App() = default;

void App::quit() { m_window->requestClose(); }

glm::mat4 App::primaryCameraView() {
    glm::mat4 view(1.0f);
    bool found = false;
    m_scene.each<Transform, Camera>([&](Entity, Transform& t, Camera& c) {
        if (found || !c.primary) return;
        found = true;
        m_renderer->settings.fovDegrees = c.fovDeg;
        view = glm::inverse(t.matrix());
    });
    if (!found) {
        // No camera in the scene: a sane default so "create a box and run"
        // shows something.
        view = glm::lookAt(glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(0.0f),
                           glm::vec3(0.0f, 1.0f, 0.0f));
    }
    return view;
}

void App::renderScene() {
    m_scene.each<Transform, MeshRenderer>(
        [&](Entity, Transform& t, MeshRenderer& mr) {
            const Mesh* mesh = m_assets.mesh(mr.meshAsset);
            if (!mesh) return; // unresolved asset: skip, never crash
            DrawItem item;
            item.mesh = mesh;
            item.model = t.matrix();
            item.color = glm::vec3(mr.color);
            item.color2 = glm::vec3(mr.color);
            if (!mr.textureAsset.empty()) {
                item.texture = m_assets.texture(mr.textureAsset);
            }
            m_renderer->draw(item);
        });
}

void App::run(const std::function<void(Frame&)>& frameFn) {
    while (!m_window->shouldClose()) {
        m_window->pollEvents();

        const double now = m_window->time();
        // Clamp so a debugger pause or window drag doesn't catapult dt.
        const float dt = std::min(float(now - m_lastTime), 0.1f);
        m_lastTime = now;

        m_imgui->beginFrame();

        // First enabled AudioSource drives the global drone; if sources exist
        // but all are disabled, the drone is muted. No sources at all leaves
        // params untouched (the app may drive Audio directly).
        if (m_audio) {
            int sources = 0;
            bool applied = false;
            m_scene.each<AudioSource>([&](Entity, AudioSource& src) {
                ++sources;
                if (applied || !src.enabled) return;
                applied = true;
                m_audio->params.gain = src.gain;
            });
            if (sources > 0) m_audio->params.enabled = applied;
        }

        m_renderer->beginFrame(primaryCameraView());

        if (frameFn) {
            Frame frame{dt,        now,        *m_window, m_scene,
                        *m_renderer, m_assets, m_audio.get(), *this};
            frameFn(frame);
        }

        // Scripts run after the user callback (so app code can stage state
        // for them) and before the built-in render (so their mutations are
        // visible the same frame).
        m_scripts->update(m_scene, dt);

        renderScene();

        // Deferred scene switch (lm.scene.change). Done at frame end so the
        // current frame finishes rendering the old scene; on success the host
        // is rebuilt so scripts start fresh against the loaded scene. A failed
        // load is reported and leaves the running scene untouched.
        if (!m_pendingScene.empty()) {
            const std::string path = m_pendingScene;
            m_pendingScene.clear();
            try {
                Scene loaded = Scene::load(path);
                m_scene = std::move(loaded);
                buildScriptHost(); // fresh instances against the new scene
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "[script] lm.scene.change failed: %s\n",
                             ex.what());
            }
        }

        int fbw = 0, fbh = 0;
        m_window->framebufferSize(fbw, fbh);
        m_renderer->endFrame(fbw, fbh);

        m_imgui->endFrame();
        m_window->swapBuffers();
    }
}

} // namespace liminal
