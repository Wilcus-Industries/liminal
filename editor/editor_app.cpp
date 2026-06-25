// EditorApp implementation. One class, panels as methods — the editor is a
// single-instance tool, not a library.

#include "editor_app.hpp"

#include "theme.hpp"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API
#include <ImGuizmo.h>
#ifdef LIMINAL_NOTO_EMOJI_TTF
#include <imgui_freetype.h> // ImGuiFreeTypeBuilderFlags_LoadColor
#endif

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <liminal/audio/audio.hpp>
#include <liminal/core/assets.hpp>
#include <liminal/core/pak.hpp>
#include <liminal/core/platform.hpp>
#include <liminal/scene/component_registry.hpp>
#include <liminal/scene/raycast.hpp>
#include <liminal/scene/serialize.hpp>
#include <liminal/version.hpp>

#include "resource_paths.hpp"

#include <nlohmann/json.hpp>
#if defined(LIMINAL_WITH_INFERENCE)
#include <liminal/inference/engine.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace liminal::editor {

namespace {

// Model matrix for a billboarded entity: keep position+scale, replace rotation
// with a camera-facing one. yawOnly -> spin about Y only; else also pitch.
glm::mat4 billboardModel(const Transform& t, const glm::vec3& camPos,
                         bool yawOnly) {
    const glm::vec3 d = camPos - t.position;
    const float yaw = std::atan2(d.x, d.z); // face the camera about +Y
    glm::mat4 m = glm::translate(glm::mat4(1.0f), t.position);
    m = glm::rotate(m, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    if (!yawOnly) {
        const float horiz = std::sqrt(d.x * d.x + d.z * d.z);
        const float pitch = -std::atan2(d.y, horiz);
        m = glm::rotate(m, pitch, glm::vec3(1.0f, 0.0f, 0.0f));
    }
    m = glm::scale(m, t.scale);
    return m;
}

// ImGui only focuses windows on left-click; focus on right-click too. Call
// right after a panel's Begin() (window body being drawn) so right-clicking a
// panel focuses it like left-click does.
void focusOnRightClick() {
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !ImGui::IsWindowFocused())
        ImGui::SetWindowFocus();
}

// Builtin asset names mirrored from <liminal/core/asset_cache.hpp> — there is
// no enumeration API on AssetCache (names are an open set), so the browser
// lists the documented fixed ones.
const char* kBuiltinMeshes[] = {
    "builtin:box",    "builtin:pyramid", "builtin:pillar", "builtin:arch",
    "builtin:stair",  "builtin:plane",   "builtin:quad",   "builtin:blob:1",
    "builtin:tree:1", "builtin:rock:1",  "builtin:crystal:1"};
const char* kBuiltinTextures[] = {
    "builtin:white",   "builtin:checker", "builtin:grid",  "builtin:noise",
    "builtin:concrete", "builtin:wood",   "builtin:metal", "builtin:brick",
    "builtin:plaster", "builtin:grass",   "builtin:dirt",  "builtin:water"};

// Resize the OS window to w x h and recenter it on the primary monitor's work
// area. Used to shrink to a compact landing/chooser window and grow back to the
// full editor on project open.
void sizeAndCenter(GLFWwindow* win, int w, int h) {
    glfwSetWindowSize(win, w, h);
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
        glfwSetWindowPos(win, mx + (mw - w) / 2, my + (mh - h) / 2);
    }
}

std::string entityLabel(entt::registry& reg, entt::entity e) {
    if (const auto* n = reg.try_get<Name>(e); n && !n->value.empty())
        return n->value;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "entity %u", entt::to_integral(e));
    return buf;
}

// Fast reject for extensions the Script Editor would only refuse via its
// content sniff anyway. Unknown extensions still go through open(), whose NUL
// check is the authoritative binary guard.
std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool isKnownBinaryExt(const std::string& extLower) {
    static const char* kBinary[] = {
        ".png", ".jpg",  ".jpeg", ".tga", ".bmp", ".dds", ".ktx", ".ttf",
        ".otf", ".wav",  ".ogg",  ".mp3", ".bin", ".glb", ".fbx", ".o",
        ".a",   ".dylib", ".so",  ".dll", ".exe"};
    for (const char* b : kBinary)
        if (extLower == b) return true;
    return false;
}

} // namespace

EditorApp::EditorApp(std::string projectFile, bool startEmpty)
    : m_window(1600, 950, "liminal editor"),
      m_renderer(),
      m_imgui(m_window) {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

#ifdef LIMINAL_EDITOR_FONT_TTF
    // JetBrains Mono as the default font (first face added wins). Size by the
    // window's content scale and shrink FontGlobalScale back so retina renders
    // crisp. fs::exists is mandatory — AddFontFromFileTTF asserts on a missing
    // file rather than returning null. A second, larger face (m_titleFont) backs
    // the "LIMINAL" watermark on the landing screen.
    {
        ImGuiIO& io = ImGui::GetIO();
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(m_window.handle(), &sx, &sy);
        const float scale = sx > 0.0f ? sx : 1.0f;
        // Prefer a bundled copy (packaged .app / portable dir), fall back to the
        // configure-time baked path for in-tree dev builds.
        const std::string fontPath = liminal::editor::resolveResource(
            "JetBrainsMono.ttf", LIMINAL_EDITOR_FONT_TTF);
        if (fs::exists(fontPath) &&
            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f * scale)) {
            io.FontGlobalScale = 1.0f / scale;
#ifdef LIMINAL_NOTO_EMOJI_TTF
            // Merge Noto Color Emoji over the body face so the terminal panel
            // (and any UI text) can show color emoji + symbol glyphs JetBrains
            // Mono lacks. Requires the FreeType atlas builder
            // (IMGUI_ENABLE_FREETYPE) + LoadColor for the CBDT color bitmaps.
            // Best-effort: skip + log if the font wasn't fetched.
            const std::string emojiPath = liminal::editor::resolveResource(
                "NotoColorEmoji.ttf", LIMINAL_NOTO_EMOJI_TTF);
            if (fs::exists(emojiPath)) {
                // Wide range covers symbols + emoji planes; ImWchar is 32-bit
                // here (IMGUI_USE_WCHAR32), so plane-1 codepoints fit. In 1.92's
                // dynamic font system ranges are advisory, but we keep an
                // explicit one for clarity. FontLoaderFlags is the 1.92 rename of
                // the old FontBuilderFlags.
                static const ImWchar kEmojiRanges[] = {0x1, 0x1FFFF, 0};
                ImFontConfig cfg;
                cfg.MergeMode = true;
                cfg.OversampleH = cfg.OversampleV = 1;
                cfg.FontLoaderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
                io.Fonts->AddFontFromFileTTF(emojiPath.c_str(), 16.0f * scale,
                                             &cfg, kEmojiRanges);
            } else {
                log("[editor] Noto Color Emoji not found, emoji glyphs disabled");
            }
#endif
            m_titleFont =
                io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 34.0f * scale);
        } else {
            log("[editor] JetBrains Mono not found, using default font");
        }
    }
#endif

    // Apply the active theme over ImGuiLayer's base StyleColorsDark. Runs after
    // the font block so it's the last word on style at startup.
    applyTheme(m_themeName);

    // Seed the default one-of-each dock panel set (Hierarchy..ScriptEditor).
    // Must exist before the first drawUi and before openProject below (so its
    // terminal(s) pick up the working dir).
    seedDefaultPanels();

    registerBuiltinComponents();
    m_gizmoOp = ImGuizmo::TRANSLATE;
    // Seed the Camera-inspector shader catalog with the built-ins so the
    // dropdown works before any project (with shaders/) is opened. openProject
    // -> scanShaders rebuilds it with discovered user shaders.
    shaderCatalog() = {"native", "retro"};
    log("[editor] liminal editor started");

    // Screen selection: a project path opens straight into the editor; --empty
    // opens a blank editor scene; otherwise show the landing / project chooser.
    if (!projectFile.empty()) {
        openProject(projectFile); // flips m_screen -> Editor on success
    } else if (startEmpty) {
        m_screen = Screen::Editor;
    } else {
        m_screen = Screen::Landing;
        m_recents = loadRecentProjects();
        // Compact chooser window (half the editor size); grows back on open.
        sizeAndCenter(m_window.handle(), 800, 475);
    }
}

EditorApp::~EditorApp() = default;

// --- loop --------------------------------------------------------------------

void EditorApp::run() {
    double last = m_window.time();
    while (!m_window.shouldClose()) {
        m_window.pollEvents();
        const double now = m_window.time();
        m_dt = std::min(float(now - last), 0.1f);
        last = now;

        // While a script holds the cursor captured in Play, the OS cursor is
        // GLFW_CURSOR_DISABLED (hidden + locked) but glfwGetCursorPos still
        // reports unbounded virtual motion, which the ImGui backend feeds to
        // io.MousePos — so ImGui would hover/highlight panels behind the game.
        // ConfigFlags_NoMouse makes NewFrame ignore the mouse entirely. Must be
        // set before beginFrame() (NewFrame reads it). The free-cursor case is
        // handled separately by confineCursorToViewport(). Toggle only this bit.
        {
            ImGuiIO& io = ImGui::GetIO();
            const bool gameOwnsMouse =
                m_mode == Mode::Play && !m_paused && m_window.cursorCaptured();
            if (gameOwnsMouse) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
            else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        }

        m_imgui.beginFrame();
        ImGuizmo::BeginFrame();

        if (m_screen == Screen::Landing) {
            // No scene/FBO work before a project is open: the landing screen is
            // pure ImGui over a cleared backbuffer.
            drawLanding();
            m_imgui.endFrame();
            m_window.swapBuffers();
            continue;
        }

        // Drain MCP tool tasks on the main thread so they read scene state
        // safely (the http threads parked them here — see mcp_server.hpp).
        if (m_mcp) m_mcp->pump();

        // Hot-reload custom shaders edited on disk (throttled ~0.5s inside).
        tickShaderWatch();

        if (m_mode == Mode::Play && !m_paused && m_scripts)
            m_scripts->update(m_scene, m_dt);

        // Render the scene into the renderer's low-res FBO. endFrame's blit to
        // the backbuffer is fully covered by the dockspace; the viewport panel
        // shows colorTexture() instead.
        m_view = currentView();
        int fbw = 0, fbh = 0;
        m_window.framebufferSize(fbw, fbh);
        m_renderer.beginFrame(m_view, fbw, fbh);
        // projection() derives aspect from the render size resolved inside
        // beginFrame, so fetch it after beginFrame.
        m_proj = m_renderer.projection();
        renderScene();
        m_renderer.endFrame(fbw, fbh);

        // Cleared each frame; drawViewport re-stamps it. If the Viewport panel
        // isn't drawn, the empty rect makes confineCursorToViewport a no-op.
        m_viewportMaxX = m_viewportMinX = 0.0f;
        m_viewportMaxY = m_viewportMinY = 0.0f;
        drawUi();
        confineCursorToViewport();

        m_imgui.endFrame();
        m_window.swapBuffers();
    }
}

// --- landing screen ----------------------------------------------------------

namespace {

const char* const kRepoUrl = "https://github.com/wilcus-industries/liminal";

// Collapse a leading $HOME to "~" for compact display of project paths.
std::string collapseHome(const std::string& path) {
    if (const char* home = std::getenv("HOME")) {
        const std::string h = home;
        if (!h.empty() && path.rfind(h, 0) == 0)
            return "~" + path.substr(h.size());
    }
    return path;
}

// 1–2 uppercase initials from a title for the project avatar (JetBrains-style).
std::string avatarInitials(const std::string& title) {
    std::string out;
    bool wordStart = true;
    for (char c : title) {
        const bool alnum = std::isalnum(static_cast<unsigned char>(c)) != 0;
        if (!alnum) { wordStart = true; continue; }
        if (wordStart) {
            out += char(std::toupper(static_cast<unsigned char>(c)));
            if (out.size() >= 2) break;
            wordStart = false;
        }
    }
    if (out.empty()) out = "?";
    return out;
}

// Deterministic avatar fill color from the project path (FNV-1a -> hue palette).
ImU32 avatarColor(const std::string& key) {
    std::uint32_t h = 2166136261u;
    for (char c : key) { h ^= std::uint8_t(c); h *= 16777619u; }
    static const ImU32 palette[] = {
        IM_COL32(0x6c, 0x5c, 0xe7, 255), IM_COL32(0x00, 0x98, 0x88, 255),
        IM_COL32(0xd6, 0x3a, 0x6a, 255), IM_COL32(0x2d, 0x7d, 0xd2, 255),
        IM_COL32(0x8e, 0x44, 0xad, 255), IM_COL32(0xc0, 0x7f, 0x2c, 255),
        IM_COL32(0x44, 0x8a, 0x44, 255), IM_COL32(0xb0, 0x3a, 0x3a, 255)};
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

} // namespace

void EditorApp::drawLanding() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##liminal-landing", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float leftW = std::max(320.0f, avail.x * 0.34f);
    bool wantCreate = false;
    std::string pendingOpen;        // a recent row clicked this frame
    std::string pendingRemove;      // a recent row's "remove" clicked

    // --- left column: watermark + actions + repo/version footer ---
    ImGui::BeginChild("##landing-left", ImVec2(leftW, avail.y), false);
    {
        const float pad = 28.0f;
        ImGui::Dummy(ImVec2(0, 26));
        ImGui::Indent(pad);

        if (m_titleFont) ImGui::PushFont(m_titleFont);
        ImGui::TextUnformatted("LIMINAL");
        if (m_titleFont) ImGui::PopFont();
        ImGui::TextDisabled("game engine");

        ImGui::Dummy(ImVec2(0, 28));

        // Action list — append to grow (e.g. a future "Settings" entry).
        struct LandingAction {
            const char* label;
            std::function<void()> on;
        };
        std::vector<LandingAction> actions = {
            {"Create new project", [&] { wantCreate = true; }},
            {"Quit", [&] { m_window.requestClose(); }},
        };
        const float btnW = leftW - pad * 2.0f;
        for (const auto& a : actions) {
            if (ImGui::Button(a.label, ImVec2(btnW, 36))) a.on();
            ImGui::Dummy(ImVec2(0, 6));
        }

        ImGui::Unindent(pad);

        // Footer pinned to the bottom: git-branch glyph + version/commit, the
        // whole row a link to the repo (glyph + text turn blue on hover).
        const float lineH = ImGui::GetTextLineHeight();
        const float footerH = lineH + 8.0f;
        ImGui::SetCursorPosY(avail.y - footerH - 16.0f);
        ImGui::SetCursorPosX(pad);

        std::string label = std::string("v") + kVersionString;
        if (std::string(kGitCommit) != "unknown")
            label += "  " + std::string(kGitCommit);

        const ImVec2 textSz = ImGui::CalcTextSize(label.c_str());
        const float iconSz = lineH;
        const float gap = 8.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##repo-link",
                               ImVec2(iconSz + gap + textSz.x, iconSz));
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) openUrl(kRepoUrl);

        const ImU32 col =
            hov ? IM_COL32(80, 150, 255, 255)
                : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // git-branch icon: a trunk (two nodes) with one branch node off the top.
        const float r = iconSz * 0.13f;
        const ImVec2 a(p.x + r * 1.6f, p.y + r * 1.6f);            // top trunk
        const ImVec2 b(p.x + r * 1.6f, p.y + iconSz - r * 1.6f);   // bottom trunk
        const ImVec2 c(p.x + iconSz - r * 1.6f, p.y + r * 1.6f);   // branch node
        dl->AddLine(a, b, col, 1.6f);
        dl->AddLine(ImVec2(a.x, (a.y + b.y) * 0.5f), c, col, 1.6f);
        dl->AddCircleFilled(a, r, col);
        dl->AddCircleFilled(b, r, col);
        dl->AddCircleFilled(c, r, col);
        dl->AddText(ImVec2(p.x + iconSz + gap, p.y + (iconSz - textSz.y) * 0.5f),
                    col, label.c_str());
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 0);

    // --- right column: recent projects ---
    ImGui::BeginChild("##landing-right", ImVec2(0, avail.y), false);
    {
        const float pad = 28.0f;
        ImGui::Dummy(ImVec2(0, 26));
        ImGui::Indent(pad);
        ImGui::TextUnformatted("Recent projects");
        ImGui::Dummy(ImVec2(0, 10));

        if (m_recents.empty()) {
            ImGui::TextDisabled("No recent projects yet.");
            ImGui::TextDisabled("Create one to get started.");
        }

        const float rowW = ImGui::GetContentRegionAvail().x - pad;
        const float lineH = ImGui::GetTextLineHeight();
        const float rowH = lineH * 2.0f + 16.0f;
        for (std::size_t i = 0; i < m_recents.size(); ++i) {
            const RecentProject& rp = m_recents[i];
            const bool exists = fs::exists(rp.path);
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            // Full-row selectable; manual content drawn on top.
            if (ImGui::Selectable("##row", false,
                                  ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(rowW, rowH)) &&
                exists)
                pendingOpen = rp.path;
            if (ImGui::BeginPopupContextItem("##row-ctx")) {
                if (ImGui::MenuItem("Remove from list")) pendingRemove = rp.path;
                ImGui::EndPopup();
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float av = rowH - 16.0f;
            const ImVec2 ap(rowStart.x + 8.0f, rowStart.y + 8.0f);
            const ImU32 acol =
                exists ? avatarColor(rp.path) : IM_COL32(90, 90, 90, 255);
            dl->AddRectFilled(ap, ImVec2(ap.x + av, ap.y + av), acol, 7.0f);
            const std::string ini = avatarInitials(rp.title);
            const ImVec2 inSz = ImGui::CalcTextSize(ini.c_str());
            dl->AddText(ImVec2(ap.x + (av - inSz.x) * 0.5f,
                               ap.y + (av - inSz.y) * 0.5f),
                        IM_COL32_WHITE, ini.c_str());

            const float tx = ap.x + av + 14.0f;
            const ImU32 titleCol =
                exists ? ImGui::GetColorU32(ImGuiCol_Text)
                       : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            dl->AddText(ImVec2(tx, rowStart.y + 8.0f), titleCol,
                        rp.title.c_str());
            const std::string sub =
                exists ? collapseHome(rp.path) : collapseHome(rp.path) + "  (missing)";
            // Truncate the path's tail with "..." when it overruns the row.
            const float availW = (rowStart.x + rowW) - tx - 8.0f;
            std::string display = sub;
            bool truncated = false;
            if (ImGui::CalcTextSize(sub.c_str()).x > availW) {
                truncated = true;
                std::size_t n = sub.size();
                while (n > 3 &&
                       ImGui::CalcTextSize((sub.substr(0, n) + "...").c_str()).x >
                           availW)
                    --n;
                display = sub.substr(0, n) + "...";
            }
            const ImVec2 pPos(tx, rowStart.y + 8.0f + lineH + 4.0f);
            dl->AddText(pPos, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        display.c_str());
            if (truncated) {
                const ImVec2 pSz = ImGui::CalcTextSize(display.c_str());
                if (ImGui::IsMouseHoveringRect(
                        pPos, ImVec2(pPos.x + availW, pPos.y + pSz.y)))
                    ImGui::SetTooltip("%s", sub.c_str());
            }
            ImGui::PopID();
        }
        ImGui::Unindent(pad);
    }
    ImGui::EndChild();

    // --- Create Project modal ---
    if (wantCreate) {
        if (const char* home = std::getenv("HOME"))
            std::snprintf(m_projectPathBuf, sizeof(m_projectPathBuf), "%s", home);
        m_newProjectNameBuf[0] = '\0';
        ImGui::OpenPopup("Create Project");
    }
    if (ImGui::BeginPopupModal("Create Project", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Parent directory:");
        ImGui::SetNextItemWidth(480);
        ImGui::InputText("##parentdir", m_projectPathBuf, sizeof(m_projectPathBuf));
        ImGui::TextUnformatted("Project name:");
        ImGui::SetNextItemWidth(480);
        ImGui::InputText("##projname", m_newProjectNameBuf,
                         sizeof(m_newProjectNameBuf));
        const bool ready =
            m_projectPathBuf[0] != '\0' && m_newProjectNameBuf[0] != '\0';
        if (ready) {
            const fs::path dest =
                fs::path(m_projectPathBuf) / m_newProjectNameBuf;
            ImGui::TextDisabled("Creates %s", dest.string().c_str());
        }
        ImGui::Dummy(ImVec2(0, 4));
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button("Create")) {
            createProject(m_projectPathBuf, m_newProjectNameBuf);
            ImGui::CloseCurrentPopup();
        }
        if (!ready) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();

    // Apply deferred actions after the window closes (openProject rebuilds a lot
    // of state; mutating m_recents mid-iteration would be unsafe).
    if (!pendingRemove.empty()) {
        removeRecentProject(pendingRemove);
        m_recents = loadRecentProjects();
    }
    if (!pendingOpen.empty()) openProject(pendingOpen);
}

void EditorApp::createProject(const std::string& parentDir,
                              const std::string& name) {
    if (name.empty()) {
        log("[editor] create project: name required");
        return;
    }
    const fs::path root = fs::path(parentDir) / name;
    std::error_code ec;
    fs::create_directories(root / "scenes", ec);
    if (ec) {
        log("[editor] create project failed: " + ec.message());
        return;
    }

    // project.ljson — assetRoot is the project dir; startupScene relative to it.
    {
        nlohmann::json pj = {{"assetRoot", "."},
                             {"startupScene", "scenes/main.lscene"},
                             {"title", name}};
        std::ofstream out(root / "project.ljson");
        if (!out) {
            log("[editor] create project: cannot write project.ljson");
            return;
        }
        out << pj.dump(4) << '\n';
    }

    // Default scene: a primary camera looking at a textured cube + a light.
    {
        Scene s;
        Entity cam = s.create("camera");
        cam.add(Transform{.position = {0.0f, 2.0f, 6.0f},
                          .rotationEuler = {-12.0f, 0.0f, 0.0f}});
        cam.add(Camera{}); // primary by default, "native" shader
        Entity cube = s.create("cube");
        cube.add(Transform{.position = {0.0f, 0.5f, 0.0f}});
        cube.add(MeshRenderer{}); // builtin:box
        Entity light = s.create("light");
        light.add(Transform{.position = {3.0f, 5.0f, 2.0f}});
        light.add(Light{});
        if (!s.save((root / "scenes" / "main.lscene").string()))
            log("[editor] create project: failed to write default scene");
    }

    log("[editor] created project: " + root.string());
    openProject(root.string()); // flips to the editor + records the recent
}

glm::mat4 EditorApp::currentView() {
    // Select the active shader pack from the first primary camera in BOTH edit
    // and play mode, so the viewport previews the chosen shader even when not
    // playing. Defaults to "native" when there is no primary camera.
    {
        bool shaderFound = false;
        m_scene.each<Transform, Camera>([&](Entity, Transform&, Camera& c) {
            if (shaderFound || !c.primary) return;
            shaderFound = true;
            m_renderer.useShaderPack(c.shaderName);
        });
        if (!shaderFound) m_renderer.useShaderPack("native");
    }
    if (m_mode == Mode::Play) {
        bool found = false;
        glm::mat4 view(1.0f);
        m_scene.each<Transform, Camera>([&](Entity, Transform& t, Camera& c) {
            if (found || !c.primary) return;
            found = true;
            m_renderer.settings.fovDegrees = c.fovDeg;
            view = glm::inverse(t.matrix());
        });
        if (found) return view;
        // No primary camera: fall through to the editor camera.
    }
    m_renderer.settings.fovDegrees = 70.0f;
    const float yaw = glm::radians(m_camYaw);
    const float pitch = glm::radians(m_camPitch);
    const glm::vec3 fwd(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                        std::cos(pitch) * std::cos(yaw));
    return glm::lookAt(m_camPos, m_camPos + fwd, glm::vec3(0, 1, 0));
}

void EditorApp::renderScene() {
    m_scene.each<Transform, MeshRenderer>(
        [&](Entity e, Transform& t, MeshRenderer& mr) {
            const Mesh* mesh = m_assets.mesh(mr.meshAsset);
            if (!mesh) return; // unresolved asset: skip, never crash
            DrawItem item;
            item.mesh = mesh;
            item.model = e.has<Billboard>()
                             ? billboardModel(t, m_camPos, e.get<Billboard>().yawOnly)
                             : t.matrix();
            item.color = glm::vec3(mr.color);
            item.color2 = glm::vec3(mr.color2);
            item.texture = m_assets.texture(
                mr.textureAsset.empty() ? "builtin:white" : mr.textureAsset);
            m_renderer.draw(item);
        });
}

// --- panel registry ----------------------------------------------------------

std::unique_ptr<TerminalPanel> EditorApp::makeTerminal() {
    auto t = std::make_unique<TerminalPanel>();
    // A terminal spawned while a project is open must start in the project dir.
    if (!m_projectFile.empty())
        t->setWorkingDir(fs::path(m_projectFile).parent_path().string());
    return t;
}

std::unique_ptr<ScriptEditorPanel> EditorApp::makeScriptEditor() {
    return std::make_unique<ScriptEditorPanel>(
        [this](const std::string& line) { log(line); });
}

EditorApp::PanelInstance& EditorApp::addPanel(PanelKind kind) {
    PanelInstance inst;
    inst.kind = kind;
    inst.uid = m_nextPanelUid++;
    if (kind == PanelKind::Terminal)
        inst.terminal = makeTerminal();
    else if (kind == PanelKind::ScriptEditor)
        inst.scriptEditor = makeScriptEditor();
    m_panels.push_back(std::move(inst));
    return m_panels.back();
}

void EditorApp::seedDefaultPanels() {
    // Tear down any existing terminals first (stop pty + reader thread).
    for (auto& p : m_panels)
        if (p.kind == PanelKind::Terminal && p.terminal) p.terminal->stopSession();
    m_panels.clear();
    m_nextPanelUid = 1;
    // Fixed startup uids 1..7 — buildDefaultLayout docks by the matching title.
    addPanel(PanelKind::Hierarchy);    // ##1
    addPanel(PanelKind::Inspector);    // ##2
    addPanel(PanelKind::Viewport);     // ##3
    addPanel(PanelKind::AssetBrowser); // ##4
    addPanel(PanelKind::Console);      // ##5
    addPanel(PanelKind::Terminal);     // ##6
    addPanel(PanelKind::ScriptEditor); // ##7
    m_activeViewportUid = 3;
}

bool EditorApp::anyScriptEditorDirty() const {
    for (const auto& p : m_panels)
        if (p.kind == PanelKind::ScriptEditor && p.scriptEditor &&
            p.scriptEditor->anyDirty())
            return true;
    return false;
}

bool EditorApp::anyScriptEditorFocused() const {
    for (const auto& p : m_panels)
        if (p.kind == PanelKind::ScriptEditor && p.scriptEditor &&
            p.scriptEditor->focused())
            return true;
    return false;
}

bool EditorApp::anyTerminalFocused() const {
    for (const auto& p : m_panels)
        if (p.kind == PanelKind::Terminal && p.terminal && p.terminal->focused())
            return true;
    return false;
}

ScriptEditorPanel* EditorApp::scriptEditorForOpen(int& uid) {
    ScriptEditorPanel* first = nullptr;
    int firstUid = 0;
    for (auto& p : m_panels) {
        if (p.kind != PanelKind::ScriptEditor || !p.scriptEditor) continue;
        if (!first) { first = p.scriptEditor.get(); firstUid = p.uid; }
        if (p.scriptEditor->focused()) { uid = p.uid; return p.scriptEditor.get(); }
    }
    uid = firstUid;
    return first; // may be null -> caller defers a spawn
}

// --- dockspace / panels ------------------------------------------------------

void EditorApp::drawUi() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    const ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##liminal-editor-host", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockId = ImGui::GetID("LiminalDockSpace");
    if (ImGui::DockBuilderGetNode(dockId) == nullptr) buildDefaultLayout(dockId);
    ImGui::DockSpace(dockId);

    drawMenuBar(); // may append panels (Tools menu) — safe: before the loop
    ImGui::End();

    // Pick the active viewport (owns camera/pick/cursor-confine) for this frame.
    // If the stored one was closed, fall back to the first Viewport instance.
    {
        bool activeExists = false;
        int firstViewport = 0;
        for (const auto& p : m_panels) {
            if (p.kind != PanelKind::Viewport) continue;
            if (!firstViewport) firstViewport = p.uid;
            if (p.uid == m_activeViewportUid) activeExists = true;
        }
        if (!activeExists) m_activeViewportUid = firstViewport;
    }

    // Draw every panel. Index iteration (not range-for): a panel's draw may
    // append to m_panels (Asset Browser defer-open is queued, Tools menu ran
    // pre-loop, so in practice nothing appends here — but indices stay valid
    // across an append regardless, whereas a reference/iterator would not).
    for (std::size_t i = 0; i < m_panels.size(); ++i) {
        PanelInstance& inst = m_panels[i];
        switch (inst.kind) {
            case PanelKind::Hierarchy:    drawHierarchy(inst); break;
            case PanelKind::Inspector:    drawInspector(inst); break;
            case PanelKind::Viewport:     drawViewport(inst); break;
            case PanelKind::AssetBrowser: drawAssetBrowser(inst); break;
            case PanelKind::Console:      drawConsole(inst); break;
            case PanelKind::Terminal:     drawTerminal(inst); break;
            case PanelKind::ScriptEditor: drawScriptEditor(inst); break;
        }
    }

    // Erase windows closed via their X this frame. Terminals tear down their
    // pty/reader thread first.
    for (std::size_t i = 0; i < m_panels.size();) {
        if (m_panels[i].open) { ++i; continue; }
        if (m_panels[i].kind == PanelKind::Terminal && m_panels[i].terminal)
            m_panels[i].terminal->stopSession();
        m_panels.erase(m_panels.begin() + std::ptrdiff_t(i));
    }

    // Deferred Asset-Browser open with no ScriptEditor present: spawn one now
    // (outside the loop, so the m_panels append can't dangle a draw reference).
    if (!m_pendingOpenInNewScriptEditor.empty()) {
        PanelInstance& se = addPanel(PanelKind::ScriptEditor);
        se.scriptEditor->open(m_pendingOpenInNewScriptEditor);
        m_pendingOpenInNewScriptEditor.clear();
    }

    // Coalesce continuous widget edits (gizmo drag, inspector DragFloat3) into
    // single undo entries. Edit mode only — Play edits are transient. Runs after
    // every panel drew, so the scene reflects this frame's interaction.
    if (m_mode == Mode::Edit) {
        const bool interacting =
            ImGui::IsAnyItemActive() || ImGuizmo::IsUsing();
        m_history.tick(m_scene, interacting);
    }
}

void EditorApp::drawScriptEditor(PanelInstance& inst) {
    if (!inst.scriptEditor) return;
    const std::string title = "Script Editor##" + std::to_string(inst.uid);
    inst.scriptEditor->draw(m_dt, title.c_str(), &inst.open);
}

void EditorApp::drawTerminal(PanelInstance& inst) {
    if (!inst.terminal) return;
    const std::string title = "Terminal##" + std::to_string(inst.uid);
    inst.terminal->draw(title.c_str(), &inst.open);
}

void EditorApp::buildDefaultLayout(unsigned int dockspaceId) {
    ImGuiID dockId = dockspaceId;
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockId;
    const ImGuiID left =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    const ImGuiID right =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);

    // Dock by the default instances' title-with-uid strings (seedDefaultPanels
    // hands out the fixed uids 1..7).
    ImGui::DockBuilderDockWindow("Hierarchy##1", left);
    ImGui::DockBuilderDockWindow("Inspector##2", right);
    ImGui::DockBuilderDockWindow("Asset Browser##4", bottom);
    ImGui::DockBuilderDockWindow("Console##5", bottom);
    ImGui::DockBuilderDockWindow("Viewport##3", center);
    // Script Editor shares the center node, tabbed behind the Viewport.
    ImGui::DockBuilderDockWindow("Script Editor##7", center);
    // Terminal shares the center node too, tabbed behind the Viewport.
    ImGui::DockBuilderDockWindow("Terminal##6", center);
    ImGui::DockBuilderFinish(dockId);
}

void EditorApp::applyTheme(const std::string& name) {
    const theme::Theme* t = theme::find(name);
    if (!t) {
        log("[editor] unknown theme: " + name);
        return;
    }
    // Reset to stock defaults first: StyleColors* only rewrites Colors[], not
    // layout vars (WindowRounding, FramePadding, ...). Without this, switching
    // from a theme that tweaks vars to one that doesn't would leave the old
    // vars stuck. Reset makes every theme idempotent and order-independent.
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    t->apply(s);
    m_themeName = name;
}

void EditorApp::drawMenuBar() {
    bool wantOpenProject = false, wantOpenScene = false, wantSaveAs = false;
    bool wantBuild = false;
    // Close Project: deferred so OpenPopup runs outside the BeginMenu scope.
    bool wantCloseProject = false, wantCloseConfirm = false;

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene")) newScene();
            if (ImGui::MenuItem("Open Project...")) wantOpenProject = true;
            if (ImGui::MenuItem("Open Scene...")) wantOpenScene = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Save Scene", "Cmd+S")) {
                if (m_scenePath.empty())
                    wantSaveAs = true;
                else
                    saveScene(m_scenePath);
            }
            if (ImGui::MenuItem("Save Scene As...")) wantSaveAs = true;
            ImGui::Separator();
            // Close Project: confirm first if any script tab is dirty (the
            // editor has no scene-dirty tracking, so that's the only check).
            if (ImGui::MenuItem("Close Project", nullptr, false,
                                !m_projectFile.empty())) {
                if (anyScriptEditorDirty())
                    wantCloseConfirm = true;
                else
                    wantCloseProject = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) m_window.requestClose();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            const bool editMode = m_mode == Mode::Edit;
            if (ImGui::MenuItem("Undo", "Cmd+Z", false,
                                editMode && m_history.canUndo()))
                doUndo();
            if (ImGui::MenuItem("Redo", "Cmd+Y", false,
                                editMode && m_history.canRedo()))
                doRedo();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Game")) {
            if (ImGui::MenuItem("Play", nullptr, false, m_mode == Mode::Edit))
                startPlay();
            if (ImGui::MenuItem("Pause", nullptr, m_paused, m_mode == Mode::Play))
                m_paused = !m_paused;
            if (ImGui::MenuItem("Stop", nullptr, false, m_mode == Mode::Play))
                stopPlay();
            ImGui::Separator();
            const bool canBuild = m_mode == Mode::Edit && !m_assetRoot.empty() &&
                                  !m_projectFile.empty();
            if (ImGui::MenuItem("Build Game...", nullptr, false, canBuild))
                wantBuild = true;
            if (!canBuild && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Open a project and stop Play to build a game.");
            ImGui::EndMenu();
        }
        // Theme menu — one caller of applyTheme; a future settings window is
        // another. No menu-specific switching logic lives here.
        if (ImGui::BeginMenu("Theme")) {
            for (const auto& t : theme::registry())
                if (ImGui::MenuItem(t.name.c_str(), nullptr, m_themeName == t.name))
                    applyTheme(t.name);
            ImGui::EndMenu();
        }
        // Tools — spawn a new (closable) instance of any panel. Runs before the
        // panel draw loop, so appending to m_panels here is safe.
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("New Hierarchy"))     addPanel(PanelKind::Hierarchy);
            if (ImGui::MenuItem("New Inspector"))     addPanel(PanelKind::Inspector);
            if (ImGui::MenuItem("New Viewport"))      addPanel(PanelKind::Viewport);
            if (ImGui::MenuItem("New Asset Browser")) addPanel(PanelKind::AssetBrowser);
            if (ImGui::MenuItem("New Console"))       addPanel(PanelKind::Console);
            if (ImGui::MenuItem("New Terminal"))      addPanel(PanelKind::Terminal);
            if (ImGui::MenuItem("New Script Editor")) addPanel(PanelKind::ScriptEditor);
            ImGui::EndMenu();
        }
        ImGui::TextDisabled("| %s%s",
                            m_scenePath.empty() ? "(unsaved scene)"
                                                : m_scenePath.c_str(),
                            m_mode == Mode::Play ? "  [PLAY]" : "");
        ImGui::EndMenuBar();
    }

    // Global Cmd/Ctrl+S: the Script Editor saves its active tab while its
    // window is focused (it handles the chord itself); everywhere else the
    // chord saves the scene. WantTextInput is excluded so typing a path into
    // a modal can't trigger a stale-path scene save.
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool mod = io.KeySuper || io.KeyCtrl;
        const bool scriptFocused = anyScriptEditorFocused();
        if (mod && ImGui::IsKeyPressed(ImGuiKey_S, false) && !scriptFocused &&
            !io.WantTextInput) {
            if (m_scenePath.empty())
                wantSaveAs = true;
            else
                saveScene(m_scenePath);
        }

        // Undo/redo (Edit mode scene edits). The Script Editor and Terminal own
        // their own undo while focused (TextEditor's UndoBuffer; the shell), so
        // skip the global chord for them. Cmd+Y or Cmd+Shift+Z = redo.
        const bool terminalFocused = anyTerminalFocused();
        if (m_mode == Mode::Edit && !scriptFocused && !terminalFocused &&
            !io.WantTextInput && mod) {
            const bool shift = io.KeyShift;
            if (!shift && ImGui::IsKeyPressed(ImGuiKey_Z, false))
                doUndo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
                     (shift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))
                doRedo();
        }
    }

    // Close Project: the no-confirm path runs straight away (deferred out of
    // the menu scope); the dirty path queues the discard-confirm modal.
    if (wantCloseProject) closeProject();
    if (wantCloseConfirm) ImGui::OpenPopup("Discard unsaved changes?");

    // Modals (single-instance editor: function-local request -> popup).
    if (wantOpenProject) {
        std::snprintf(m_projectPathBuf, sizeof(m_projectPathBuf), "%s",
                      m_projectFile.c_str());
        ImGui::OpenPopup("Open Project");
    }
    if (wantOpenScene) {
        std::snprintf(m_scenePathBuf, sizeof(m_scenePathBuf), "%s",
                      m_scenePath.c_str());
        ImGui::OpenPopup("Open Scene");
    }
    if (wantSaveAs) {
        std::snprintf(m_scenePathBuf, sizeof(m_scenePathBuf), "%s",
                      m_scenePath.c_str());
        ImGui::OpenPopup("Save Scene As");
    }
    if (wantBuild) {
        // Default output: <projectDir>/../<name>-build/<name>(.exe). Derive the
        // game name from the project folder. All via fs::path for portability.
        const fs::path projDir = fs::path(m_projectFile).parent_path();
        std::string name = projDir.filename().string();
        if (name.empty()) name = "game";
        fs::path out = projDir.parent_path() / (name + "-build") / name;
        std::snprintf(m_buildPathBuf, sizeof(m_buildPathBuf), "%s",
                      out.lexically_normal().string().c_str());
        ImGui::OpenPopup("Build Game");
    }

    if (ImGui::BeginPopupModal("Open Project", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path to project.ljson (or its directory):");
        ImGui::SetNextItemWidth(520);
        ImGui::InputText("##projpath", m_projectPathBuf, sizeof(m_projectPathBuf));
        if (ImGui::Button("Open")) {
            openProject(m_projectPathBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Open Scene", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Scene path (absolute, or relative to asset root):");
        ImGui::SetNextItemWidth(520);
        ImGui::InputText("##scnpath", m_scenePathBuf, sizeof(m_scenePathBuf));
        if (ImGui::Button("Open")) {
            openScene(m_scenePathBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save Scene As", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Write .lscene to:");
        ImGui::SetNextItemWidth(520);
        ImGui::InputText("##savepath", m_scenePathBuf, sizeof(m_scenePathBuf));
        if (ImGui::Button("Save")) {
            saveScene(m_scenePathBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Build Game", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "Output game executable (a platform suffix is added as needed):");
        ImGui::SetNextItemWidth(520);
        ImGui::InputText("##buildpath", m_buildPathBuf, sizeof(m_buildPathBuf));
        if (ImGui::Button("Build")) {
            buildGame(m_buildPathBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Discard unsaved changes?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "You have unsaved script changes. Close the project anyway?");
        if (ImGui::Button("Discard & Close")) {
            closeProject();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorApp::drawHierarchy(PanelInstance& inst) {
    const std::string title = "Hierarchy##" + std::to_string(inst.uid);
    ImGui::Begin(title.c_str(), &inst.open);
    focusOnRightClick();

    if (ImGui::Button("+ Empty")) {
        m_history.pushSnapshot(m_scene);
        Entity e = m_scene.create("entity");
        e.add<Transform>({});
        m_selected = e.handle();
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Box")) {
        m_history.pushSnapshot(m_scene);
        Entity e = m_scene.create("box");
        e.add<Transform>({});
        e.add<MeshRenderer>({});
        m_selected = e.handle();
    }

    entt::registry& reg = m_scene.registry();
    const bool hasSel = m_selected != entt::null && reg.valid(m_selected);
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button("Duplicate")) {
        m_history.pushSnapshot(m_scene);
        Entity dup = duplicateEntity(m_selected);
        m_selected = dup.handle();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        m_history.pushSnapshot(m_scene);
        m_scene.destroy(Entity(m_selected, &m_scene));
        m_selected = entt::null;
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    std::vector<entt::entity> entities;
    for (auto e : reg.view<entt::entity>()) entities.push_back(e);
    std::sort(entities.begin(), entities.end(),
              [](entt::entity a, entt::entity b) {
                  return entt::to_integral(a) < entt::to_integral(b);
              });

    for (auto e : entities) {
        ImGui::PushID(int(entt::to_integral(e)));
        if (ImGui::Selectable(entityLabel(reg, e).c_str(), m_selected == e))
            m_selected = e;
        ImGui::PopID();
    }
    ImGui::End();
}

void EditorApp::drawInspector(PanelInstance& inst) {
    const std::string title = "Inspector##" + std::to_string(inst.uid);
    ImGui::Begin(title.c_str(), &inst.open);
    focusOnRightClick();
    entt::registry& reg = m_scene.registry();
    if (m_selected == entt::null || !reg.valid(m_selected)) {
        ImGui::TextDisabled("no selection");
        ImGui::End();
        return;
    }

    const auto& allOps = ComponentRegistry::instance().all();
    for (const auto& ops : allOps) {
        if (!ops.has(reg, m_selected)) continue;
        ImGui::PushID(ops.name.c_str());
        bool keep = true;
        if (ImGui::CollapsingHeader(ops.name.c_str(), &keep,
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (void* c = ops.getRaw(reg, m_selected); c && ops.inspectorDraw)
                ops.inspectorDraw(c);
            else
                ImGui::TextDisabled("(no inspector)");
        }
        if (!keep) { // header close button = remove
            m_history.pushSnapshot(m_scene);
            ops.removeFrom(reg, m_selected);
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::BeginCombo("##addcomp", "Add component...")) {
        for (const auto& ops : allOps) {
            if (ops.has(reg, m_selected)) continue;
            if (ImGui::Selectable(ops.name.c_str())) {
                // Empty object -> every field takes its default (built-in
                // fromJson uses json::value with defaults throughout).
                m_history.pushSnapshot(m_scene);
                ops.fromJson(reg, m_selected, nlohmann::json::object());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::End();
}

void EditorApp::drawViewport(PanelInstance& inst) {
    const std::string title = "Viewport##" + std::to_string(inst.uid);
    ImGui::Begin(title.c_str(), &inst.open);
    focusOnRightClick();

    // Only ONE viewport drives camera/pick/cursor-confine. isActive is decided
    // from m_activeViewportUid (resolved at the top of drawUi). Focusing a
    // viewport claims ownership for the NEXT frame — computed AFTER isActive so
    // grabbing focus never double-handles input the same frame. All viewports
    // share the single renderer FBO, so non-active ones are passive mirrors.
    const bool isActive = (inst.uid == m_activeViewportUid);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        m_activeViewportUid = inst.uid;

    // Toolbar: play controls + undo/redo + gizmo mode.
    if (m_mode == Mode::Edit) {
        if (ImGui::Button("Play")) startPlay();
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_history.canUndo());
        if (ImGui::Button("Undo")) doUndo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_history.canRedo());
        if (ImGui::Button("Redo")) doRedo();
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button(m_paused ? "Resume" : "Pause")) m_paused = !m_paused;
        ImGui::SameLine();
        if (ImGui::Button("Stop")) stopPlay();
    }
    ImGui::SameLine();
    ImGui::RadioButton("move (W)", &m_gizmoOp, ImGuizmo::TRANSLATE);
    ImGui::SameLine();
    ImGui::RadioButton("rotate (E)", &m_gizmoOp, ImGuizmo::ROTATE);
    ImGui::SameLine();
    ImGui::RadioButton("scale (R)", &m_gizmoOp, ImGuizmo::SCALE);
    ImGui::SameLine();
    ImGui::TextDisabled("RMB fly, ctrl snaps");

    // Letterbox the low-res render target to its own aspect so picking and
    // gizmo math line up with renderer.projection().
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float aspect = float(m_renderer.settings.virtualW) /
                         float(m_renderer.settings.virtualH);
    ImVec2 size(avail.x, avail.x / aspect);
    if (size.y > avail.y) size = ImVec2(avail.y * aspect, avail.y);
    if (size.x < 16.0f || size.y < 16.0f) { // collapsed panel
        ImGui::End();
        return;
    }
    // Record the Viewport window rect for confineCursorToViewport (Play mode) —
    // only for the active viewport (the one input is bound to this frame).
    // Whole window, not just the image, so the Play/Pause/Stop toolbar stays
    // clickable while other docked panels are walled off.
    if (isActive) {
        const ImVec2 wpos = ImGui::GetWindowPos();
        const ImVec2 wsize = ImGui::GetWindowSize();
        m_viewportMinX = wpos.x;
        m_viewportMinY = wpos.y;
        m_viewportMaxX = wpos.x + wsize.x;
        m_viewportMaxY = wpos.y + wsize.y;
    }

    ImVec2 cursor = ImGui::GetCursorPos();
    cursor.x += (avail.x - size.x) * 0.5f;
    cursor.y += (avail.y - size.y) * 0.5f;
    ImGui::SetCursorPos(cursor);

    const ImVec2 imgMin = ImGui::GetCursorScreenPos();
    // FBO texture is bottom-up; flip V.
    ImGui::Image(ImTextureID(uintptr_t(m_renderer.colorTexture())), size,
                 ImVec2(0, 1), ImVec2(1, 0));
    const bool hovered = isActive && ImGui::IsItemHovered();
    const bool clicked = isActive && ImGui::IsItemClicked(ImGuiMouseButton_Left);

    // Camera fly, gizmo, picking — only the active viewport. Non-active windows
    // are passive mirrors of the shared FBO (an accepted limitation: all
    // viewports show the same camera view).
    if (isActive) handleCameraInput(hovered);

    // Scroll-to-adjust camera-speed indicator. Editor-only (lives in the
    // editor viewport draw); fades over its last 0.4s. Set in handleCameraInput.
    if (isActive && m_camSpeedHud > 0.0f) {
        m_camSpeedHud -= m_dt;
        const float alpha = std::clamp(m_camSpeedHud / 0.4f, 0.0f, 1.0f);
        char buf[48];
        std::snprintf(buf, sizeof buf, "Camera speed  %.1f", m_camSpeed);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 pad(10.0f, 6.0f);
        ImVec2 tp(imgMin.x + (size.x - ts.x) * 0.5f,
                  imgMin.y + size.y - ts.y - 24.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(tp.x - pad.x, tp.y - pad.y),
                          ImVec2(tp.x + ts.x + pad.x, tp.y + ts.y + pad.y),
                          ImGui::GetColorU32(ImVec4(0, 0, 0, 0.55f * alpha)), 5.0f);
        dl->AddText(tp, ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), buf);
    }

    if (isActive) {
        // Gizmo mode hotkeys — never while flying or typing.
        ImGuiIO& io = ImGui::GetIO();
        if (hovered && !io.WantTextInput && !m_window.cursorCaptured()) {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmoOp = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOp = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOp = ImGuizmo::SCALE;
        }

        const bool gizmoDrawn =
            drawGizmo(glm::vec2(imgMin.x, imgMin.y), glm::vec2(size.x, size.y));

        if (clicked &&
            (!gizmoDrawn || (!ImGuizmo::IsOver() && !ImGuizmo::IsUsing()))) {
            const ImVec2 mouse = ImGui::GetMousePos();
            pickEntity(glm::vec2((mouse.x - imgMin.x) / size.x,
                                 (mouse.y - imgMin.y) / size.y));
        }
    }

    ImGui::End();
}

void EditorApp::handleCameraInput(bool viewportHovered) {
    // Drain the accumulated wheel delta every frame, before any early-return.
    // scrollDelta() zeroes-on-read, so leaving it un-drained during Play or
    // while the cursor is over another panel buffers that scroll and dumps it
    // as a sudden camera-speed jump the next time the viewport is hovered.
    const float scroll = m_window.scrollDelta();

    // Play: scripts own the cursor/camera (like the standalone player) — don't
    // stomp a script-driven setCursorCaptured. RMB-fly is an Edit-mode tool.
    if (m_mode != Mode::Edit) return;
    const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);

    // Scroll adjusts fly speed whenever the viewport is hovered (no RMB needed),
    // and also while actively flying (capture hides the cursor so hover reads
    // false). Apply only when the viewport is the intended target so the drained
    // value tweaks speed + shows the HUD; stale scroll from elsewhere is dropped.
    if (viewportHovered || m_window.cursorCaptured()) {
        if (scroll != 0.0f) {
            m_camSpeed =
                std::clamp(m_camSpeed * (1.0f + scroll * 0.12f), 0.5f, 60.0f);
            m_camSpeedHud = 1.2f; // show the speed indicator, then fade
        }
    }

    if (viewportHovered && rmb && !m_window.cursorCaptured())
        m_window.setCursorCaptured(true);
    if (!rmb && m_window.cursorCaptured()) m_window.setCursorCaptured(false);
    if (!m_window.cursorCaptured()) return;

    float dx = 0, dy = 0;
    m_window.mouseDelta(dx, dy);
    m_camYaw -= dx * 0.18f;
    m_camPitch = std::clamp(m_camPitch - dy * 0.18f, -89.0f, 89.0f);

    const float yaw = glm::radians(m_camYaw);
    const float pitch = glm::radians(m_camPitch);
    const glm::vec3 fwd(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                        std::cos(pitch) * std::cos(yaw));
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
    glm::vec3 move(0.0f);
    if (m_window.keyDown(GLFW_KEY_W)) move += fwd;
    if (m_window.keyDown(GLFW_KEY_S)) move -= fwd;
    if (m_window.keyDown(GLFW_KEY_D)) move += right;
    if (m_window.keyDown(GLFW_KEY_A)) move -= right;
    if (m_window.keyDown(GLFW_KEY_E)) move.y += 1.0f;
    if (m_window.keyDown(GLFW_KEY_Q)) move.y -= 1.0f;
    if (glm::dot(move, move) > 0.0f)
        m_camPos += glm::normalize(move) * m_camSpeed * m_dt;
}

void EditorApp::confineCursorToViewport() {
    // Only while actively playing with a free (visible) cursor. A script-
    // captured cursor is already GLFW_CURSOR_DISABLED (locked); pause leaves the
    // cursor free on purpose so the user can poke editor panels.
    if (m_mode != Mode::Play || m_paused || m_window.cursorCaptured()) return;
    if (m_viewportMaxX <= m_viewportMinX || m_viewportMaxY <= m_viewportMinY)
        return; // viewport not drawn this frame
    GLFWwindow* w = m_window.handle();
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    const double cx = std::clamp(x, double(m_viewportMinX), double(m_viewportMaxX));
    const double cy = std::clamp(y, double(m_viewportMinY), double(m_viewportMaxY));
    if (cx != x || cy != y) glfwSetCursorPos(w, cx, cy);
}

bool EditorApp::drawGizmo(const glm::vec2& imgMin, const glm::vec2& imgSize) {
    entt::registry& reg = m_scene.registry();
    if (m_selected == entt::null || !reg.valid(m_selected)) return false;
    auto* t = reg.try_get<Transform>(m_selected);
    if (!t) return false;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(imgMin.x, imgMin.y, imgSize.x, imgSize.y);

    // Round-trip through ImGuizmo's own recompose/decompose so the gizmo and
    // the inspector agree. (ImGuizmo composes euler XYZ; Transform::matrix
    // applies Y->X->Z — single-axis rotations match exactly, compound ones
    // are approximated. Fine for an editor gizmo.)
    float model[16];
    ImGuizmo::RecomposeMatrixFromComponents(&t->position.x, &t->rotationEuler.x,
                                            &t->scale.x, model);

    static const float snapT[3] = {0.5f, 0.5f, 0.5f};
    static const float snapR[3] = {15.0f, 15.0f, 15.0f};
    static const float snapS[3] = {0.25f, 0.25f, 0.25f};
    const float* snap = nullptr;
    if (ImGui::GetIO().KeyCtrl) {
        snap = (m_gizmoOp == ImGuizmo::ROTATE)  ? snapR
               : (m_gizmoOp == ImGuizmo::SCALE) ? snapS
                                                : snapT;
    }

    if (ImGuizmo::Manipulate(glm::value_ptr(m_view), glm::value_ptr(m_proj),
                             ImGuizmo::OPERATION(m_gizmoOp), ImGuizmo::LOCAL,
                             model, nullptr, snap)) {
        ImGuizmo::DecomposeMatrixToComponents(model, &t->position.x,
                                              &t->rotationEuler.x, &t->scale.x);
    }
    return true;
}

void EditorApp::pickEntity(const glm::vec2& uv) {
    const glm::mat4 inv = glm::inverse(m_proj * m_view);
    glm::vec4 p0 = inv * glm::vec4(uv.x * 2 - 1, 1 - uv.y * 2, -1, 1);
    glm::vec4 p1 = inv * glm::vec4(uv.x * 2 - 1, 1 - uv.y * 2, 1, 1);
    p0 /= p0.w;
    p1 /= p1.w;
    const glm::vec3 origin(p0);
    const glm::vec3 dir = glm::normalize(glm::vec3(p1) - origin);

    // AABB pass via the shared raycast (Collider or mesh bounds).
    entt::entity best = entt::null;
    float bestAlong = 1e9f;
    if (auto hit = raycastScene(m_scene, m_assets, origin, dir, 0.0f)) {
        best = hit->entity;
        bestAlong = hit->distance;
    }

    // Sphere-proxy fallback for entities the AABB pass can't see (no mesh and
    // no collider) — a nearer proxy hit still wins.
    entt::registry& reg = m_scene.registry();
    m_scene.each<Transform>([&](Entity e, Transform& t) {
        const bool hasMesh = [&] {
            const auto* mr = reg.try_get<MeshRenderer>(e.handle());
            return mr && m_assets.mesh(mr->meshAsset);
        }();
        if (hasMesh || reg.all_of<Collider>(e.handle()))
            return; // handled above
        const glm::vec3 d = t.position - origin;
        const float along = glm::dot(d, dir);
        if (along < 0.0f) return;
        const float perp = glm::length(d - along * dir);
        const float radius = std::max(
            0.5f, 0.75f * std::max({t.scale.x, t.scale.y, t.scale.z}));
        if (perp < radius && along < bestAlong) {
            bestAlong = along;
            best = e.handle();
        }
    });
    m_selected = best; // miss = deselect
}

void EditorApp::drawAssetBrowser(PanelInstance& inst) {
    const std::string winTitle = "Asset Browser##" + std::to_string(inst.uid);
    ImGui::Begin(winTitle.c_str(), &inst.open);
    focusOnRightClick();

    if (ImGui::Button("Refresh")) refreshAssetTree();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_assetRoot.empty() ? "(no project open)"
                                                  : m_assetRoot.c_str());
    ImGui::Separator();

    // Recursive draw of the cached fs tree.
    std::function<void(const FsEntry&)> drawEntry = [&](const FsEntry& e) {
        if (e.isDir) {
            if (ImGui::TreeNode(e.name.c_str())) {
                for (const auto& child : e.children) drawEntry(child);
                ImGui::TreePop();
            }
            return;
        }
        const fs::path p(e.path);
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (ImGui::Selectable(e.name.c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            if (ext == ".lscene") {
                openScene(e.path);
            } else if (isKnownBinaryExt(ext)) {
                m_browserStatus = e.path;
            } else {
                // Any other file: try to edit it. open() sniffs content and
                // refuses binaries with an unknown extension. Route to the
                // focused Script Editor (else the first); if none exist, defer a
                // spawn to after the panel loop (spawning here would reallocate
                // m_panels mid-iteration).
                if (dbl) {
                    int seUid = 0;
                    if (ScriptEditorPanel* se = scriptEditorForOpen(seUid)) {
                        se->open(e.path); // opens + focuses the pane
                        const std::string seTitle =
                            "Script Editor##" + std::to_string(seUid);
                        ImGui::SetWindowFocus(seTitle.c_str());
                    } else {
                        m_pendingOpenInNewScriptEditor = e.path;
                    }
                } else {
                    m_browserStatus = "file: " + e.path +
                                      " (double-click to edit)";
                    log("[editor] file selected: " + e.path +
                        " (double-click opens it in the Script Editor)");
                }
            }
        }
    };
    for (const auto& child : m_assetTree.children) drawEntry(child);

    if (ImGui::CollapsingHeader("builtin: meshes")) {
        for (const char* name : kBuiltinMeshes) {
            if (ImGui::Selectable(name)) {
                ImGui::SetClipboardText(name);
                m_browserStatus = std::string("copied to clipboard: ") + name;
            }
        }
    }
    if (ImGui::CollapsingHeader("builtin: textures")) {
        for (const char* name : kBuiltinTextures) {
            if (ImGui::Selectable(name)) {
                ImGui::SetClipboardText(name);
                m_browserStatus = std::string("copied to clipboard: ") + name;
            }
        }
    }

    if (!m_browserStatus.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_browserStatus.c_str());
    }
    ImGui::End();
}

void EditorApp::drawConsole(PanelInstance& inst) {
    const std::string title = "Console##" + std::to_string(inst.uid);
    ImGui::Begin(title.c_str(), &inst.open);
    focusOnRightClick();
    if (ImGui::Button("Clear")) m_console.clear();
    ImGui::Separator();
    ImGui::BeginChild("##consolelines", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(int(m_console.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            ImGui::TextUnformatted(m_console[size_t(i)].c_str());
    }
    clipper.End();
    if (m_consoleScrollDown) {
        ImGui::SetScrollHereY(1.0f);
        m_consoleScrollDown = false;
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- commands ----------------------------------------------------------------

void EditorApp::openProject(const std::string& projectFile) {
    fs::path p = fs::absolute(projectFile);
    if (fs::is_directory(p)) p /= "project.ljson";

    std::ifstream in(p);
    if (!in) {
        log("[editor] cannot open project: " + p.string());
        return;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        log("[editor] bad project.ljson: " + std::string(e.what()));
        return;
    }

    m_projectFile = p.string();
    const fs::path root =
        (p.parent_path() / j.value("assetRoot", ".")).lexically_normal();
    m_assetRoot = root.string();
    Assets::addSearchPath(m_assetRoot);
    // Title defaults to the project folder name; startupScene kept verbatim for
    // the Build step (it's relative to assetRoot in project.ljson).
    m_projectTitle = j.value("title", p.parent_path().filename().string());
    m_startupScene = j.value("startupScene", "");
    refreshAssetTree();
    log("[editor] project open: " + m_projectFile + " (asset root " +
        m_assetRoot + ")");

    if (!m_startupScene.empty()) openScene(m_startupScene);

    // Point every editor terminal at the project dir so `claude` (or the
    // fallback shell) starts with the opened project as its CWD. No-op once a
    // given terminal has spawned. Terminals spawned later inherit it via
    // makeTerminal().
    for (auto& pnl : m_panels)
        if (pnl.kind == PanelKind::Terminal && pnl.terminal)
            pnl.terminal->setWorkingDir(p.parent_path().string());

    // Seed the liminal-lua Claude Code skill into this project if absent, so
    // Claude Code in the editor terminal knows the `lm` API. Non-fatal.
    seedLuaSkill();

    // Discover custom shaders under <projectDir>/shaders/ and (re)build the
    // Camera-inspector shader catalog. Built-ins are always present.
    scanShaders();

    // Bring up the MCP endpoint and (re)write .mcp.json now that the project
    // path / dir are known. Non-fatal if it fails (logged inside).
    startMcpServer();

    // Remember this project for the landing screen, and leave the chooser.
    recordRecentProject(m_projectFile, m_projectTitle);
    // Grow back to the full editor size if we came from the compact chooser.
    if (m_screen == Screen::Landing) sizeAndCenter(m_window.handle(), 1600, 950);
    m_screen = Screen::Editor;
}

void EditorApp::closeProject() {
    if (m_mode == Mode::Play) stopPlay();

    // Drop the MCP server (dtor stops + joins its thread).
    m_mcp.reset();

    // Tear down ALL panel instances and re-seed the default one-of-each set
    // (terminals get their pty/reader thread stopped first inside
    // seedDefaultPanels). Drops every script-editor tab (dirtiness was already
    // confirmed away by the caller) and resets uids to the fixed defaults.
    seedDefaultPanels();

    // Clear scene + selection.
    m_scene = Scene();
    m_scenePath.clear();
    m_selected = entt::null;
    m_history.clear();

    // Clear project state.
    m_projectFile.clear();
    m_assetRoot.clear();
    m_projectTitle.clear();
    m_startupScene.clear();
    m_assetTree = FsEntry{};

    // Reset the shader catalog to the built-ins and drop discovered watches.
    shaderCatalog() = {"native", "retro"};
    m_shaderWatch.clear();

    // Reload recents and shrink back to the landing chooser.
    m_recents = loadRecentProjects();
    m_screen = Screen::Landing;
    sizeAndCenter(m_window.handle(), 800, 475);

    log("[editor] project closed");
}

void EditorApp::newScene() {
    if (m_mode == Mode::Play) stopPlay();
    m_scene = Scene();
    m_scenePath.clear();
    m_selected = entt::null;
    m_history.clear();
    log("[editor] new scene");
}

void EditorApp::openScene(const std::string& path) {
    if (m_mode == Mode::Play) stopPlay();
    const std::string resolved = Assets::resolve(path);
    try {
        m_scene = Scene::load(resolved);
    } catch (const std::exception& e) {
        log("[editor] open scene failed: " + std::string(e.what()));
        return;
    }
    m_scenePath = resolved;
    m_selected = entt::null;
    m_history.clear();
    log("[editor] scene open: " + resolved + " (" +
        std::to_string(m_scene.entityCount()) + " entities)");
}

bool EditorApp::saveScene(const std::string& path) {
    if (path.empty()) return false;
    if (!m_scene.save(path)) {
        log("[editor] save FAILED: " + path);
        return false;
    }
    m_scenePath = path;
    log("[editor] scene saved: " + path);
    return true;
}

void EditorApp::buildGame(const std::string& outPath) {
    if (m_assetRoot.empty() || m_projectFile.empty()) {
        log("[build] no project open");
        return;
    }
    log("[build] building game -> " + outPath);

    // 1. Locate the prebuilt player beside the editor.
#if defined(_WIN32)
    const char* playerName = "liminal-player.exe";
#else
    const char* playerName = "liminal-player";
#endif
    const fs::path player = fs::path(selfExeDir()) / playerName;
    std::error_code ec;
    if (!fs::exists(player, ec)) {
        log("[build] liminal-player not found beside editor (" + player.string() +
            "); build the liminal-player target first");
        return;
    }

    // 2. Output exe path (ensure .exe on Windows) + parent dirs.
    fs::path outExe = fs::path(outPath);
#if defined(_WIN32)
    if (toLower(outExe.extension().string()) != ".exe")
        outExe.replace_extension(".exe");
#endif
    fs::create_directories(outExe.parent_path(), ec);
    if (ec) {
        log("[build] cannot create output directory: " + ec.message());
        return;
    }

    // 3-4. Collect assets + synthesized project.ljson into a pak. Skip the
    // build output dir if it happens to sit under the asset root.
    PakWriter pak;
    const auto logFn = [this](const std::string& m) { log(m); };
    if (!buildGamePak(m_assetRoot, m_startupScene, m_projectTitle, pak,
                      outExe.parent_path().string(), logFn)) {
        log("[build] failed to collect project assets");
        return;
    }

    // 5. Copy player -> outExe, then append the pak.
    try {
        fs::copy_file(player, outExe, fs::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        log("[build] copy player failed: " + std::string(e.what()));
        return;
    }
    bool sidecar = false;
    if (!pak.appendTo(outExe.string())) {
        log("[build] failed to append pak to " + outExe.string());
        return;
    }
#if !defined(_WIN32)
    fs::permissions(outExe,
                    fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    fs::perm_options::add, ec);
    if (ec) log("[build] warning: could not set exec bit: " + ec.message());
#endif

    // 6. Platform integrity. macOS: appending bytes to a signed binary breaks
    // its signature; re-sign ad-hoc. If that fails, fall back to a sidecar pak
    // beside a clean (unmodified, still-valid) player copy.
#if defined(__APPLE__)
    // The path is wrapped in double quotes for the shell; a literal '"' in the
    // output path would break the quoting (and could splice the command). Skip
    // signing and fall back to the sidecar path rather than run a malformed
    // command. (Such a path is pathological — '"' is legal but near-unheard-of
    // in a build output location.)
    const int rc = (outExe.string().find('"') != std::string::npos)
                       ? -1
                       : std::system(("codesign --force -s - \"" +
                                      outExe.string() + "\"")
                                         .c_str());
    if (rc != 0) {
        log("[build] codesign failed (rc=" + std::to_string(rc) +
            "); falling back to sidecar pak");
        try {
            fs::copy_file(player, outExe, fs::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            log("[build] clean player re-copy failed: " + std::string(e.what()));
            return;
        }
#if !defined(_WIN32)
        fs::permissions(outExe,
                        fs::perms::owner_exec | fs::perms::group_exec |
                            fs::perms::others_exec,
                        fs::perm_options::add, ec);
#endif
        // Sidecar path must match the player's lookup: <exeDir>/<exeStem>.pak.
        const fs::path side =
            outExe.parent_path() / (outExe.stem().string() + ".pak");
        { std::ofstream(side.string(), std::ios::binary | std::ios::trunc); }
        if (!pak.appendTo(side.string())) {
            log("[build] failed to write sidecar pak: " + side.string());
            return;
        }
        sidecar = true;
        log("[build] sidecar pak: " + side.string());
    } else {
        log("[build] codesign ok (ad-hoc); pak appended to exe");
    }
#else
    log("[build] pak appended to exe");
#endif

    // 7. Copy .gguf models beside the exe (they ship next to it, not in the pak).
    for (fs::recursive_directory_iterator it(fs::path(m_assetRoot),
             fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        if (toLower(it->path().extension().string()) != ".gguf") continue;
        const fs::path dst = outExe.parent_path() / it->path().filename();
        std::error_code cec;
        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, cec);
        if (cec)
            log("[build] warning: copy model failed: " + it->path().filename().string());
        else
            log("[build] model: " + it->path().filename().string());
    }

    // 8. Done.
    log("[build] complete: " + outExe.string() +
        (sidecar ? " (sidecar pak mode)" : " (appended pak mode)"));
}

Entity EditorApp::duplicateEntity(entt::entity src) {
    entt::registry& reg = m_scene.registry();
    Entity dup = m_scene.create(); // bare entity; components copied below
    for (const auto& ops : ComponentRegistry::instance().all()) {
        if (ops.has(reg, src))
            ops.fromJson(reg, dup.handle(), ops.toJson(reg, src));
    }
    if (auto* n = reg.try_get<Name>(dup.handle())) n->value += " copy";
    return dup;
}

void EditorApp::startPlay() {
    if (m_mode == Mode::Play) return;
    m_playSnapshot = sceneToJson(m_scene);
    m_history.clear(); // Play edits are transient; don't pollute edit-mode undo
    m_mode = Mode::Play;
    m_paused = false;
    // Editor-owned audio: created on first Play, re-enabled on subsequent ones
    // (Stop only mutes — see stopPlay — to dodge device restart churn). A
    // device that fails to open leaves m_audio null (CI/headless); the lm.audio
    // bindings null-guard, so Play still runs without sound.
    if (!m_audio) {
        auto a = std::make_unique<Audio>(1);
        if (a->ok()) m_audio = std::move(a);
    }
    if (m_audio) m_audio->params.enabled = true;
    // Fresh host per play session: clean Lua state, on_start re-runs,
    // parked-error memory wiped.
    ScriptContext ctx;
    ctx.input = &m_window;
    ctx.audio = m_audio.get();
    ctx.assets = &m_assets;
    ctx.renderer = &m_renderer;
#if defined(LIMINAL_WITH_INFERENCE)
    // Cheap to construct (no worker until lm.ai.start); created on first Play
    // and reused. stopPlay stops it to free model memory.
    if (!m_inference) m_inference = std::make_unique<inference::Engine>();
    ctx.inference = m_inference.get();
#endif
    ctx.requestSceneChange = [this](const std::string&) {
        log("[script] lm.scene.change unsupported in editor Play");
    };
    m_scripts = std::make_unique<ScriptHost>(std::move(ctx));
    m_scripts->setErrorSink(
        [this](const std::string& msg) { log("[lua] " + msg); });
    m_scripts->setLogSink(
        [this](const std::string& msg) { log("[lua] " + msg); });
    log("[editor] play");
}

void EditorApp::stopPlay() {
    if (m_mode != Mode::Play) return;
    m_scripts.reset();
    m_window.setCursorCaptured(false); // a script may have left the cursor locked
#if defined(LIMINAL_WITH_INFERENCE)
    // Release model memory between play sessions; the engine object persists
    // and start()s again on the next lm.ai.start.
    if (m_inference) m_inference->stop();
#endif
    if (m_audio) m_audio->params.enabled = false; // mute, keep device alive
    m_scene = Scene();
    sceneFromJson(m_scene, m_playSnapshot);
    m_playSnapshot = nullptr;
    m_mode = Mode::Edit;
    m_paused = false;
    m_selected = entt::null; // entity ids changed on restore
    m_history.clear();       // fresh baseline after the snapshot restore
    log("[editor] stop — scene restored from snapshot");
}

void EditorApp::selectByName(const std::string& name) {
    m_selected = entt::null;
    if (name.empty()) return;
    entt::registry& reg = m_scene.registry();
    for (auto e : reg.view<entt::entity>()) {
        if (const auto* n = reg.try_get<Name>(e); n && n->value == name) {
            m_selected = e;
            return;
        }
    }
}

void EditorApp::doUndo() {
    if (m_mode != Mode::Edit) return;
    // Remember the selection by Name — entt-ids are reset by sceneFromJson.
    std::string selName;
    if (entt::registry& reg = m_scene.registry();
        m_selected != entt::null && reg.valid(m_selected))
        if (const auto* n = reg.try_get<Name>(m_selected)) selName = n->value;
    if (!m_history.undo(m_scene)) return;
    selectByName(selName);
    log("[editor] undo");
}

void EditorApp::doRedo() {
    if (m_mode != Mode::Edit) return;
    std::string selName;
    if (entt::registry& reg = m_scene.registry();
        m_selected != entt::null && reg.valid(m_selected))
        if (const auto* n = reg.try_get<Name>(m_selected)) selName = n->value;
    if (!m_history.redo(m_scene)) return;
    selectByName(selName);
    log("[editor] redo");
}

void EditorApp::log(const std::string& line) {
    m_console.push_back(line);
    if (m_console.size() > 2000)
        m_console.erase(m_console.begin(),
                        m_console.begin() + ptrdiff_t(m_console.size() - 1000));
    m_consoleScrollDown = true;
}

// --- MCP server --------------------------------------------------------------

entt::entity EditorApp::resolveEntity(const std::string& idOrName) {
    auto& reg = m_scene.registry();
    // Resolve key as an entt id first, then fall back to a Name match.
    try {
        const auto raw = static_cast<entt::id_type>(std::stoul(idOrName));
        const auto cand = entt::entity{raw};
        if (reg.valid(cand)) return cand;
    } catch (...) {
        // not numeric — fall through to name lookup
    }
    for (auto e : reg.view<entt::entity>()) {
        const auto* n = reg.try_get<Name>(e);
        if (n && n->value == idOrName) return e;
    }
    return entt::null;
}

void EditorApp::startMcpServer() {
    if (m_projectFile.empty()) return;
    if (m_mcp) {
        // Project re-opened: refresh .mcp.json against the existing endpoint.
        writeMcpJson(m_mcp->port());
        return;
    }

    // The provider getters all run inside McpServer::pump() on the main thread
    // (see mcp_server.hpp), so capturing `this` and touching m_scene / project
    // fields from them is safe — no off-thread entt access.
    McpProvider provider;

    provider.sceneTree = [this]() -> nlohmann::json {
        registerBuiltinComponents();
        const auto& reg = m_scene.registry();
        const auto& ops = ComponentRegistry::instance().all();
        nlohmann::json entities = nlohmann::json::array();
        std::vector<entt::entity> sorted;
        for (auto e : reg.view<entt::entity>()) sorted.push_back(e);
        std::sort(sorted.begin(), sorted.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        for (auto e : sorted) {
            nlohmann::json comps = nlohmann::json::array();
            for (const auto& op : ops)
                if (op.has(reg, e)) comps.push_back(op.name);
            std::string name;
            if (const auto* n = reg.try_get<Name>(e)) name = n->value;
            entities.push_back({{"id", entt::to_integral(e)},
                                {"name", name},
                                {"components", comps}});
        }
        return {{"entities", entities}};
    };

    provider.getEntity = [this](const std::string& key) -> nlohmann::json {
        registerBuiltinComponents();
        auto& reg = m_scene.registry();
        const auto& ops = ComponentRegistry::instance().all();

        const entt::entity target = resolveEntity(key);
        if (target == entt::null) return nullptr;

        nlohmann::json comps = nlohmann::json::object();
        for (const auto& op : ops)
            if (op.has(reg, target)) comps[op.name] = op.toJson(reg, target);
        return {{"id", entt::to_integral(target)}, {"components", comps}};
    };

    provider.setComponent = [this](const std::string& idOrName,
                                   const std::string& comp,
                                   const nlohmann::json& data) -> nlohmann::json {
        registerBuiltinComponents();
        const entt::entity e = resolveEntity(idOrName);
        if (e == entt::null) return {{"error", "entity not found"}};
        const ComponentOps* ops = ComponentRegistry::instance().find(comp);
        if (!ops) return {{"error", "unknown component: " + comp}};
        m_history.pushSnapshot(m_scene); // Claude-driven edit, still undoable
        ops->fromJson(m_scene.registry(), e, data);
        return {{"ok", true},
                {"id", (int)entt::to_integral(e)},
                {"component", comp}};
    };

    provider.removeComponent = [this](const std::string& idOrName,
                                      const std::string& comp) -> nlohmann::json {
        registerBuiltinComponents();
        const entt::entity e = resolveEntity(idOrName);
        if (e == entt::null) return {{"error", "entity not found"}};
        const ComponentOps* ops = ComponentRegistry::instance().find(comp);
        if (!ops) return {{"error", "unknown component: " + comp}};
        m_history.pushSnapshot(m_scene);
        ops->removeFrom(m_scene.registry(), e);
        return {{"ok", true},
                {"id", (int)entt::to_integral(e)},
                {"component", comp}};
    };

    provider.createEntity = [this](const std::string& name) -> nlohmann::json {
        registerBuiltinComponents();
        m_history.pushSnapshot(m_scene);
        Entity ent = m_scene.create(name);
        return {{"ok", true},
                {"id", (int)entt::to_integral(ent.handle())},
                {"name", name}};
    };

    provider.destroyEntity = [this](const std::string& idOrName) -> nlohmann::json {
        registerBuiltinComponents();
        const entt::entity e = resolveEntity(idOrName);
        if (e == entt::null) return {{"error", "entity not found"}};
        if (e == m_selected) m_selected = entt::null;
        const int id = (int)entt::to_integral(e);
        m_history.pushSnapshot(m_scene);
        m_scene.destroy(Entity(e, &m_scene));
        return {{"ok", true}, {"id", id}};
    };

    provider.currentProject = [this]() -> nlohmann::json {
        return {{"projectFile", m_projectFile},
                {"title", m_projectTitle},
                {"scenePath", m_scenePath}};
    };

    provider.projectDir = [this]() -> std::string {
        if (m_projectFile.empty()) return {};
        return fs::path(m_projectFile).parent_path().string();
    };

    provider.consoleLog = [this](int lines) -> nlohmann::json {
        if (lines <= 0) lines = 200;
        const std::size_t n =
            std::min(static_cast<std::size_t>(lines), m_console.size());
        nlohmann::json arr = nlohmann::json::array();
        for (std::size_t i = m_console.size() - n; i < m_console.size(); ++i)
            arr.push_back(m_console[i]);
        return {{"lines", arr}};
    };

    provider.playState = [this]() -> nlohmann::json {
        return {{"mode", m_mode == Mode::Play ? "play" : "edit"},
                {"paused", m_paused}};
    };

    provider.screenshot = [this]() -> nlohmann::json {
        std::vector<unsigned char> rgba;
        int w = 0, h = 0;
        m_renderer.readPixels(rgba, w, h);
        if (w == 0 || h == 0) return {{"error", "no framebuffer"}};
        return {{"base64", mcpEncodePngBase64(rgba, w, h)},
                {"mimeType", "image/png"}};
    };

    provider.control = [this](const std::string& action) -> nlohmann::json {
        if (action == "play") {
            if (m_mode != Mode::Play) startPlay();
        } else if (action == "stop") {
            if (m_mode == Mode::Play) stopPlay();
        } else if (action == "pause") {
            if (m_mode == Mode::Play) m_paused = true;
            else return {{"error", "not playing"}};
        } else if (action == "resume") {
            if (m_mode == Mode::Play) m_paused = false;
            else return {{"error", "not playing"}};
        } else {
            return {{"error", "unknown action: " + action}};
        }
        return {{"mode", m_mode == Mode::Play ? "play" : "edit"},
                {"paused", m_paused}};
    };

    provider.reloadScene = [this]() -> nlohmann::json {
        if (m_scenePath.empty()) return {{"error", "no scene open"}};
        openScene(m_scenePath); // auto-stops Play + reloads from disk
        return {{"ok", true}, {"scenePath", m_scenePath}};
    };

    provider.saveScene = [this](const std::string& path) -> nlohmann::json {
        const std::string target = path.empty() ? m_scenePath : path;
        if (target.empty()) return {{"error", "no scene path"}};
        if (!this->saveScene(target))
            return {{"error", "save failed: " + target}};
        return {{"ok", true}, {"path", target}};
    };

    m_mcp = std::make_unique<McpServer>(std::move(provider));
    m_mcp->setLogSink([this](const std::string& line) { log(line); });
    const int port = m_mcp->start(7717);
    if (port == 0) {
        log("[mcp] server failed to start; Claude Code scene tools unavailable");
        m_mcp.reset();
        return;
    }
    log("[mcp] server listening at http://127.0.0.1:" + std::to_string(port) +
        "/mcp");
    writeMcpJson(port);
}

void EditorApp::writeMcpJson(int port) {
    if (m_projectFile.empty() || port == 0) return;
    const fs::path mcpPath = fs::path(m_projectFile).parent_path() / ".mcp.json";

    // Merge into an existing .mcp.json so unrelated servers survive: parse what's
    // there, set/replace just the "liminal" entry, write back. Bad/missing file
    // starts from an empty object.
    nlohmann::json doc = nlohmann::json::object();
    if (std::ifstream in(mcpPath); in) {
        try {
            in >> doc;
        } catch (const std::exception&) {
            doc = nlohmann::json::object(); // clobber only an unparseable file
        }
        if (!doc.is_object()) doc = nlohmann::json::object();
    }
    if (!doc.contains("mcpServers") || !doc["mcpServers"].is_object())
        doc["mcpServers"] = nlohmann::json::object();
    doc["mcpServers"]["liminal"] = {
        {"type", "http"},
        {"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"}};

    std::ofstream out(mcpPath);
    if (!out) {
        log("[mcp] could not write " + mcpPath.string() + " (non-fatal)");
        return;
    }
    out << doc.dump(2) << '\n';
    log("[mcp] wrote " + mcpPath.string() + " (liminal -> port " +
        std::to_string(port) + ")");
}

void EditorApp::seedLuaSkill() {
    if (m_projectFile.empty()) return;
#if !defined(LIMINAL_EDITOR_LUA_SKILL)
    return;
#else
    const std::string srcPath = liminal::editor::resolveResource(
        "skills/liminal-lua/SKILL.md", LIMINAL_EDITOR_LUA_SKILL);
    if (srcPath.empty()) return;

    const fs::path dest = fs::path(m_projectFile).parent_path() /
                          ".claude" / "skills" / "liminal-lua" / "SKILL.md";
    std::error_code ec;
    if (fs::exists(dest, ec)) return; // never clobber a customized skill

    // Don't seed a project into its own canonical source (the sample project).
    if (fs::weakly_canonical(dest, ec) ==
        fs::weakly_canonical(fs::path(srcPath), ec))
        return;

    std::ifstream src(srcPath, std::ios::binary);
    if (!src) {
        log("[skill] liminal-lua source missing: " + srcPath + " (non-fatal)");
        return;
    }
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        log("[skill] could not create " + dest.parent_path().string() +
            " (non-fatal)");
        return;
    }
    std::ofstream out(dest, std::ios::binary);
    if (!out) {
        log("[skill] could not write " + dest.string() + " (non-fatal)");
        return;
    }
    out << src.rdbuf();
    log("[skill] seeded liminal-lua skill -> " + dest.string());
#endif
}

// --- custom shader discovery + hot reload -----------------------------------

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

void EditorApp::registerShaderFromDisk(const std::string& name,
                                       const fs::path& full, bool fragOnly) {
    ShaderWatch w;
    w.full = full;
    w.fragOnly = fragOnly;
    std::error_code ec;

    // Build via the shared core helpers so the frag-only wrap contract (native
    // vertex stage + native frag header) lives in one place.
    ShaderPack pack;
    if (fragOnly) {
        pack = ShaderPack::makeFragOnlyPack(slurp(full));
        w.fragMtime = fs::last_write_time(full, ec);
    } else {
        const fs::path vert = full / "scene.vert";
        const fs::path frag = full / "scene.frag";
        pack = ShaderPack::makeFullPack(slurp(vert), slurp(frag));
        w.vertMtime = fs::last_write_time(vert, ec);
        w.fragMtime = fs::last_write_time(frag, ec);
    }
    pack.label = name; // name the pack after its dir/file in compile errors

    // Registration stores source only (recompiles immediately if active — the
    // hot-reload mechanism). May throw on the active-pack recompile.
    m_renderer.registerShaderPack(name, std::move(pack));

    // Record / refresh the watch entry.
    for (auto& e : m_shaderWatch)
        if (e.first == name) {
            e.second = std::move(w);
            return;
        }
    m_shaderWatch.emplace_back(name, std::move(w));
}

void EditorApp::scanShaders() {
    // Always seed the catalog with the built-ins, even with no shaders/ dir.
    auto& catalog = shaderCatalog();
    catalog = {"native", "retro"};
    m_shaderWatch.clear();

    if (m_projectFile.empty()) return;
    const fs::path dir = fs::path(m_projectFile).parent_path() / "shaders";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return; // no shaders/ — built-ins only

    std::vector<fs::directory_entry> entries;
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
        entries.push_back(*it);
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return a.path().filename() < b.path().filename();
              });

    for (const auto& entry : entries) {
        const fs::path p = entry.path();
        if (entry.is_directory(ec)) {
            // Full pack: subdir with both scene.vert and scene.frag.
            if (!fs::exists(p / "scene.vert", ec) ||
                !fs::exists(p / "scene.frag", ec))
                continue;
            const std::string name = p.filename().string();
            try {
                registerShaderFromDisk(name, p, /*fragOnly=*/false);
                catalog.push_back(name);
                log("[shader] registered pack '" + name + "' (full)");
            } catch (const std::exception& e) {
                log("[shader] skip '" + name + "': " + e.what());
            }
        } else if (p.extension() == ".frag") {
            // Frag-only: engine wraps it. Name = file stem.
            const std::string name = p.stem().string();
            try {
                registerShaderFromDisk(name, p, /*fragOnly=*/true);
                catalog.push_back(name);
                log("[shader] registered pack '" + name + "' (frag-only)");
            } catch (const std::exception& e) {
                log("[shader] skip '" + name + "': " + e.what());
            }
        }
    }
}

void EditorApp::tickShaderWatch() {
    m_shaderReloadTimer += m_dt;
    if (m_shaderReloadTimer < 0.5f) return;
    m_shaderReloadTimer = 0.0f;
    if (m_shaderWatch.empty()) return;

    for (auto& [name, w] : m_shaderWatch) {
        std::error_code ec;
        bool changed = false;
        if (w.fragOnly) {
            const auto fm = fs::last_write_time(w.full, ec);
            if (!ec && fm != w.fragMtime) changed = true;
        } else {
            const auto vm = fs::last_write_time(w.full / "scene.vert", ec);
            bool vEc = (bool)ec;
            ec.clear();
            const auto fm = fs::last_write_time(w.full / "scene.frag", ec);
            if ((!vEc && vm != w.vertMtime) || (!ec && fm != w.fragMtime))
                changed = true;
        }
        if (!changed) continue;

        // Re-register; registerShaderPack recompiles immediately if this is the
        // active pack and throws on compile failure — catch + keep the last
        // good program (the Renderer left it untouched), log, stay running.
        try {
            registerShaderFromDisk(name, w.full, w.fragOnly);
            log("[shader] reloaded '" + name + "'");
        } catch (const std::exception& e) {
            log("[shader] reload FAILED '" + name + "': " + e.what() +
                " (kept previous)");
            // registerShaderFromDisk updates the watch entry before the throwing
            // recompile only on success; bump mtimes here so we don't re-fire
            // every tick on a persistently-broken file.
            if (w.fragOnly) {
                w.fragMtime = fs::last_write_time(w.full, ec);
            } else {
                w.vertMtime = fs::last_write_time(w.full / "scene.vert", ec);
                ec.clear();
                w.fragMtime = fs::last_write_time(w.full / "scene.frag", ec);
            }
        }
    }
}

void EditorApp::refreshAssetTree() {
    m_assetTree = FsEntry{};
    if (m_assetRoot.empty()) return;

    std::function<void(const fs::path&, FsEntry&)> walk = [&](const fs::path& dir,
                                                              FsEntry& out) {
        std::error_code ec;
        std::vector<fs::directory_entry> entries;
        for (auto it = fs::directory_iterator(dir, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            entries.push_back(*it);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const fs::directory_entry& a, const fs::directory_entry& b) {
                      const bool ad = a.is_directory(), bd = b.is_directory();
                      if (ad != bd) return ad; // dirs first
                      return a.path().filename() < b.path().filename();
                  });
        for (const auto& entry : entries) {
            FsEntry child;
            child.name = entry.path().filename().string();
            child.path = entry.path().string();
            child.isDir = entry.is_directory();
            if (child.name.empty() || child.name[0] == '.') continue; // dotfiles
            if (child.isDir) walk(entry.path(), child);
            out.children.push_back(std::move(child));
        }
    };
    walk(m_assetRoot, m_assetTree);
}

} // namespace liminal::editor
