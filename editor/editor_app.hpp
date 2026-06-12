#pragma once
// EditorApp: the liminal scene editor shell. Owns its own Window + Renderer +
// AssetCache + ImGuiLayer and runs a hand-rolled loop (App::run's built-in
// scene render + primary-camera logic would fight the editor's FBO-to-panel
// flow). Layout: dockspace over the main viewport — Hierarchy left, Inspector
// right, Viewport center, Asset Browser + Console tabbed at the bottom.
//
// Modes:
//   Edit — fly camera (RMB-drag look + WASD/QE, scroll = speed), click-select,
//          ImGuizmo translate/rotate/scale (W/E/R, ctrl = snap).
//   Play — scene snapshot to in-memory JSON, a fresh ScriptHost ticks Script
//          components, the scene's primary Camera drives the viewport
//          (editor cam fallback). Stop restores the snapshot exactly.

#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <liminal/core/asset_cache.hpp>
#include <liminal/core/window.hpp>
#include <liminal/render/renderer.hpp>
#include <liminal/scene/scene.hpp>
#include <liminal/ui/imgui_layer.hpp>
#if defined(LIMINAL_WITH_SCRIPTING)
#include <liminal/script/script_host.hpp>
#endif

namespace liminal::editor {

class EditorApp {
public:
    // projectFile may be empty (editor opens with an empty scene, no project).
    explicit EditorApp(std::string projectFile);
    void run();

private:
    enum class Mode { Edit, Play };

    // --- frame stages ---
    void renderScene();           // each<Transform, MeshRenderer> -> DrawItem
    glm::mat4 currentView();      // editor cam, or primary Camera in Play
    void drawUi();

    // --- panels ---
    void drawMenuBar();
    void drawHierarchy();
    void drawInspector();
    void drawViewport();
    void drawAssetBrowser();
    void drawConsole();
    void buildDefaultLayout(unsigned int dockspaceId);

    // --- viewport helpers ---
    void handleCameraInput(bool viewportHovered);
    void pickEntity(const glm::vec2& uv); // uv in [0,1] over the image
    void drawGizmo(const glm::vec2& imgMin, const glm::vec2& imgSize);

    // --- commands ---
    void openProject(const std::string& projectFile);
    void newScene();
    void openScene(const std::string& path); // resolved via Assets
    bool saveScene(const std::string& path);
    Entity duplicateEntity(entt::entity src);
    void startPlay();
    void stopPlay();

    void log(const std::string& line);
    void refreshAssetTree();

    // --- core (construction order matters: Window owns the GL context) ---
    Window m_window;
    Renderer m_renderer;
    AssetCache m_assets;
    ImGuiLayer m_imgui;
#if defined(LIMINAL_WITH_SCRIPTING)
    std::unique_ptr<ScriptHost> m_scripts; // alive only while playing
#endif
    Scene m_scene;

    // --- project ---
    std::string m_projectFile; // absolute path of project.ljson, "" = none
    std::string m_assetRoot;   // absolute, "" = none
    std::string m_scenePath;   // current scene file, "" = unsaved

    // --- selection / gizmo ---
    entt::entity m_selected = entt::null;
    int m_gizmoOp = 0; // ImGuizmo::OPERATION; int to keep the header light

    // --- editor camera ---
    glm::vec3 m_camPos{6.0f, 4.5f, 9.0f};
    float m_camYaw = -147.0f;  // degrees; -Z at yaw 180
    float m_camPitch = -18.0f; // degrees
    float m_camSpeed = 6.0f;

    // --- play mode ---
    Mode m_mode = Mode::Edit;
    bool m_paused = false;
    nlohmann::json m_playSnapshot;

    // --- per-frame state the panels share ---
    float m_dt = 0.0f;
    glm::mat4 m_view{1.0f};
    glm::mat4 m_proj{1.0f};

    // --- console ---
    std::vector<std::string> m_console;
    bool m_consoleScrollDown = false;

    // --- asset browser ---
    struct FsEntry {
        std::string name;
        std::string path; // absolute
        bool isDir = false;
        std::vector<FsEntry> children;
    };
    FsEntry m_assetTree;
    std::string m_browserStatus; // e.g. last clicked .lua

    // --- modal path buffers ---
    char m_projectPathBuf[512] = {};
    char m_scenePathBuf[512] = {};
};

} // namespace liminal::editor
