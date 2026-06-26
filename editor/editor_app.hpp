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

#include <filesystem>
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
#include <liminal/script/script_host.hpp>

#include "script_editor.hpp"
#include "terminal_panel.hpp"
#include "mcp_server.hpp"
#include "recent_projects.hpp"
#include "edit_history.hpp"
#include "update_check.hpp"

struct ImFont;

namespace liminal { class Audio; }
#if defined(LIMINAL_WITH_INFERENCE)
namespace liminal::inference { class Engine; }
#endif

namespace liminal::editor {

class EditorApp {
public:
    // projectFile non-empty: open it and go straight to the editor. Empty +
    // startEmpty: skip the landing screen and open the editor on a blank scene
    // (the --empty CLI path). Empty + !startEmpty: show the landing / project
    // chooser screen (the default no-arg launch).
    explicit EditorApp(std::string projectFile, bool startEmpty = false);
    ~EditorApp(); // out-of-line: m_audio holds an incomplete Audio
    void run();

private:
    enum class Mode { Edit, Play };
    // Landing = the JetBrains-style project chooser shown before any project is
    // open; Editor = the dockspace. openProject success flips Landing -> Editor.
    enum class Screen { Landing, Editor };

    // --- frame stages ---
    void renderScene();           // each<Transform, MeshRenderer> -> DrawItem
    glm::mat4 currentView();      // editor cam, or primary Camera in Play
    void drawUi();
    // The landing / project chooser (Screen::Landing). Full-window: LIMINAL
    // watermark + action buttons + repo/version footer on the left, recent
    // projects on the right. Hosts the "Create Project" modal.
    void drawLanding();
    // Scaffold a new project under parentDir/name: project.ljson + a default
    // scenes/main.lscene (primary camera + cube + light), then openProject it.
    // Logs + non-fatal on filesystem errors.
    void createProject(const std::string& parentDir, const std::string& name);

    // --- theming ---
    // The single seam for switching ImGui themes. Both the Theme menu and any
    // future settings window call only this; unknown names are a no-op + log.
    void applyTheme(const std::string& name);

    // --- panels (closable + multi-instance via a PanelInstance registry) ---
    // Every dock panel is one entry in m_panels. Hierarchy/Inspector/Console/
    // AssetBrowser mirror SHARED editor state (selection, console, asset tree);
    // Terminal/ScriptEditor own their own state through the per-instance
    // unique_ptr. Window titles carry a "##<uid>" suffix so duplicates get
    // distinct ImGui ids while showing the same visible label.
    enum class PanelKind {
        Hierarchy,
        Inspector,
        Viewport,
        AssetBrowser,
        Console,
        Terminal,
        ScriptEditor
    };
    struct PanelInstance {
        PanelKind kind;
        int uid = 0;     // stable; forms the ImGui "##<uid>" suffix in the title
        bool open = true; // false after the window X is clicked -> erased
        std::unique_ptr<TerminalPanel> terminal;         // Terminal kind only
        std::unique_ptr<ScriptEditorPanel> scriptEditor; // ScriptEditor kind only
    };

    void drawMenuBar();
    void drawHierarchy(PanelInstance& inst);
    void drawInspector(PanelInstance& inst);
    void drawViewport(PanelInstance& inst);
    void drawAssetBrowser(PanelInstance& inst);
    void drawConsole(PanelInstance& inst);
    void drawScriptEditor(PanelInstance& inst);
    void drawTerminal(PanelInstance& inst);
    void buildDefaultLayout(unsigned int dockspaceId);
    // Request a layout reset: re-seed the default panels and rebuild the base
    // docking next frame (deferred — menu items run after the dockspace block).
    void resetLayout();

    // Seed the default one-of-each panel set at fixed startup uids (Hierarchy=1
    // .. ScriptEditor=7) so buildDefaultLayout's title strings dock them. Resets
    // m_panels + m_nextPanelUid; used by the ctor and closeProject.
    void seedDefaultPanels();
    // Append a panel of `kind` at the next uid, constructing its sub-panel
    // (terminal/script editor) wired identically to the defaults. Returns the
    // new instance. NEVER call while iterating m_panels (it may reallocate) —
    // the Tools menu (pre-loop) and the deferred-open path are the safe sites.
    PanelInstance& addPanel(PanelKind kind);
    // Factories that wire a fresh sub-panel exactly like the default set.
    std::unique_ptr<TerminalPanel> makeTerminal();
    std::unique_ptr<ScriptEditorPanel> makeScriptEditor();
    // Cross-instance predicates replacing the old single-panel singletons.
    bool anyScriptEditorDirty() const;
    bool anyScriptEditorFocused() const;
    bool anyTerminalFocused() const;
    // The ScriptEditor a "open this file" action targets: the focused one, else
    // the first; nullptr (and *uid=0) when none exist (caller may defer-spawn).
    ScriptEditorPanel* scriptEditorForOpen(int& uid);

    // --- viewport helpers ---
    void handleCameraInput(bool viewportHovered);
    // During Play (not paused, cursor not script-captured) clamp the OS cursor
    // to the Viewport window rect so clicks can't leak onto other editor panels.
    // No-op in Edit, while paused, or when a script has the cursor captured
    // (GLFW_CURSOR_DISABLED already locks it). Uses m_viewportRect*, set by
    // drawViewport; single OS window so ImGui screen coords == GLFW cursor coords.
    void confineCursorToViewport();
    void pickEntity(const glm::vec2& uv); // uv in [0,1] over the image
    // Returns true only when ImGuizmo::Manipulate ran this frame (IsOver/
    // IsUsing are stale otherwise).
    bool drawGizmo(const glm::vec2& imgMin, const glm::vec2& imgSize);

    // --- commands ---
    void openProject(const std::string& projectFile);
    // Tear down the open project (MCP server, terminal, script editor, scene,
    // project/shader state) and return to the landing chooser. Does not save —
    // the File-menu caller gates this behind a discard-confirm when any script
    // tab is dirty (the editor has no scene-dirty tracking).
    void closeProject();
    void newScene();
    void openScene(const std::string& path); // resolved via Assets
    bool saveScene(const std::string& path);
    // Produce a standalone game: copy liminal-player, append a pak of project
    // assets, make it runnable (codesign on macOS, exec bit on POSIX). Logs each
    // step to the console; never throws.
    void buildGame(const std::string& outPath);
    Entity duplicateEntity(entt::entity src);
    void startPlay();
    void stopPlay();

    // --- undo/redo (Edit mode scene edits) ---
    // Apply an undo/redo step: remember the current selection's Name, restore
    // the snapshot, then re-resolve selection by Name (entt-ids reset on load).
    // No-op + log when the stack is empty or while in Play.
    void doUndo();
    void doRedo();
    // Set m_selected to the first entity whose Name matches, else entt::null.
    void selectByName(const std::string& name);

    void log(const std::string& line);
    void refreshAssetTree();

    // MCP: build the read-only provider over editor state and (re)write the
    // project's .mcp.json so `claude` discovers the editor's HTTP endpoint.
    void startMcpServer();
    void writeMcpJson(int port);

    // Update check: launch the once-per-session GitHub release probe (detached),
    // and paint the resulting red "update available" warning on the menubar.
    void startUpdateCheck();
    void drawUpdateWarning();

    // Seed the canonical liminal-lua Claude Code skill into the opened
    // project's .claude/skills/ if absent (never overwrites a customized one).
    void seedLuaSkill();

    // Scan <projectDir>/shaders/ and register each discovered shader as a pack,
    // then rebuild shaderCatalog() = {"native","retro", <discovered...>}. A
    // subdir with scene.vert+scene.frag is a full pack; a lone *.frag is wrapped
    // (engine vertex + boilerplate). Records each file's mtime for hot reload.
    void scanShaders();
    // Build (or rebuild) a single pack from a discovered shader source on disk.
    // Throws std::runtime_error if a required file cannot be read; the caller
    // logs + skips. Records mtimes into m_shaderWatch under name.
    void registerShaderFromDisk(const std::string& name,
                                const std::filesystem::path& full,
                                bool fragOnly);
    // Throttled (~0.5s) mtime poll of discovered shader files; re-registers a
    // pack whose source changed (recompiles immediately if active). Compile
    // failures are caught + logged, keeping the previous good pack.
    void tickShaderWatch();

    // Resolves "123" (entt id) or a Name to an entity; entt::null if not found.
    // Used by the MCP get/set/remove/destroy entity tools (main thread only).
    entt::entity resolveEntity(const std::string& idOrName);

    // --- core (construction order matters: Window owns the GL context) ---
    Window m_window;
    Renderer m_renderer;
    AssetCache m_assets;
    ImGuiLayer m_imgui;
    std::unique_ptr<ScriptHost> m_scripts; // alive only while playing
#if defined(LIMINAL_WITH_INFERENCE)
    // Local LLM engine for lm.ai during Play. Constructed lazily on first Play;
    // stopped on Stop to release model memory (the engine object persists).
    std::unique_ptr<inference::Engine> m_inference;
#endif
    // Editor-owned audio for Play. Created lazily on first Play and kept alive
    // (muted via params.enabled on Stop) to avoid device restart churn.
    std::unique_ptr<Audio> m_audio;
    Scene m_scene;

    // --- dock panels (closable + multi-instance) -----------------------------
    // One entry per open panel window. Terminal/ScriptEditor instances own their
    // own pty/shell + TextEditor tabs; the rest mirror shared editor state.
    std::vector<PanelInstance> m_panels;
    int m_nextPanelUid = 1;       // next "##<uid>" suffix to hand out
    int m_activeViewportUid = 0;  // the Viewport instance that owns camera/pick
    bool m_resetLayout = false;   // pending Reset Layout (applied in drawUi)
    // A file double-clicked in the Asset Browser when NO ScriptEditor exists:
    // open it in a freshly spawned ScriptEditor AFTER the panel loop (spawning
    // mid-loop would reallocate m_panels). Empty = nothing pending.
    std::string m_pendingOpenInNewScriptEditor;

    // --- MCP server (read-only scene introspection for Claude Code) ---
    // Started on first project open; pump()ed each frame on the main thread so
    // its tools read m_scene / project fields safely (see mcp_server.hpp).
    std::unique_ptr<McpServer> m_mcp;

    // --- update check (background, once per session) ---
    // Kicked on the first project open; the detached worker writes the result
    // into the shared state and the menubar paints a red warning when behind.
    std::shared_ptr<UpdateCheckState> m_updateCheck;
    bool m_updateCheckStarted = false;

    // --- project ---
    std::string m_projectFile; // absolute path of project.ljson, "" = none
    std::string m_assetRoot;   // absolute, "" = none
    std::string m_scenePath;   // current scene file, "" = unsaved
    std::string m_projectTitle;   // project title (defaults to folder name)
    std::string m_startupScene;   // startupScene from project.ljson (as written)
    // Stable per-user ImGui ini path (~/.liminal/editor_layout.ini). Held as a
    // member because io.IniFilename stores the raw pointer, not a copy.
    std::string m_iniPath;

    // --- selection / gizmo ---
    entt::entity m_selected = entt::null;
    int m_gizmoOp = 0; // ImGuizmo::OPERATION; int to keep the header light

    // --- undo/redo (Edit mode scene edits; whole-scene JSON snapshots) ---
    EditHistory m_history;

    // --- editor camera ---
    glm::vec3 m_camPos{6.0f, 4.5f, 9.0f};
    float m_camYaw = -147.0f;  // degrees; -Z at yaw 180
    float m_camPitch = -18.0f; // degrees
    float m_camSpeed = 6.0f;
    float m_camSpeedHud = 0.0f; // seconds left on the scroll-to-adjust speed HUD

    // --- play mode ---
    Mode m_mode = Mode::Edit;
    bool m_paused = false;
    nlohmann::json m_playSnapshot;

    // --- per-frame state the panels share ---
    // Viewport window rect in ImGui screen coords, refreshed each frame by
    // drawViewport; max <= min means "not drawn this frame" (skip confine).
    float m_viewportMinX = 0.0f, m_viewportMinY = 0.0f;
    float m_viewportMaxX = 0.0f, m_viewportMaxY = 0.0f;
    float m_dt = 0.0f;
    glm::mat4 m_view{1.0f};
    glm::mat4 m_proj{1.0f};

    // --- landing screen ---
    Screen m_screen = Screen::Landing; // start on the chooser unless a project opens
    ImFont* m_titleFont = nullptr;     // larger JetBrains Mono face for "LIMINAL"
    std::vector<RecentProject> m_recents; // loaded once for the landing list

    // --- theming ---
    std::string m_themeName = "Liminal"; // active ImGui theme; default Liminal

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

    // --- custom shader discovery + hot reload ---
    // One discovered pack: the source file(s) on disk and their last-seen
    // mtimes. fragOnly => `full` is a *.frag wrapped by the engine; otherwise
    // `full` is a subdir holding scene.vert + scene.frag.
    struct ShaderWatch {
        std::filesystem::path full; // *.frag file (fragOnly) or pack subdir
        bool fragOnly = false;
        std::filesystem::file_time_type vertMtime{}; // unused when fragOnly
        std::filesystem::file_time_type fragMtime{};
    };
    std::vector<std::pair<std::string, ShaderWatch>> m_shaderWatch; // name -> src
    float m_shaderReloadTimer = 0.0f; // throttles the mtime poll (~0.5s)

    // --- modal path buffers ---
    char m_projectPathBuf[512] = {};
    char m_scenePathBuf[512] = {};
    char m_buildPathBuf[512] = {};
    char m_newProjectNameBuf[256] = {}; // Create Project modal: project name
};

} // namespace liminal::editor
