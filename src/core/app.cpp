#include <liminal/core/app.hpp>

#include <liminal/audio/audio.hpp>
#include <liminal/scene/component_registry.hpp>
#include <liminal/ui/imgui_layer.hpp>
#if defined(LIMINAL_WITH_SCRIPTING)
#include <liminal/script/script_host.hpp>
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

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
#if defined(LIMINAL_WITH_SCRIPTING)
    m_scripts = std::make_unique<ScriptHost>(m_window.get());
#endif
    m_lastTime = m_window->time();
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

#if defined(LIMINAL_WITH_SCRIPTING)
        // Scripts run after the user callback (so app code can stage state
        // for them) and before the built-in render (so their mutations are
        // visible the same frame).
        m_scripts->update(m_scene, dt);
#endif

        renderScene();

        int fbw = 0, fbh = 0;
        m_window->framebufferSize(fbw, fbh);
        m_renderer->endFrame(fbw, fbh);

        m_imgui->endFrame();
        m_window->swapBuffers();
    }
}

} // namespace liminal
