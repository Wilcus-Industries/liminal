// EditorApp implementation. One class, panels as methods — the editor is a
// single-instance tool, not a library.

#include "editor_app.hpp"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API
#include <ImGuizmo.h>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <liminal/core/assets.hpp>
#include <liminal/scene/component_registry.hpp>
#include <liminal/scene/serialize.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

namespace fs = std::filesystem;

namespace liminal::editor {

namespace {

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

std::string entityLabel(entt::registry& reg, entt::entity e) {
    if (const auto* n = reg.try_get<Name>(e); n && !n->value.empty())
        return n->value;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "entity %u", entt::to_integral(e));
    return buf;
}

} // namespace

EditorApp::EditorApp(std::string projectFile)
    : m_window(1600, 950, "liminal editor"),
      m_renderer(),
      m_imgui(m_window) {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

#ifdef LIMINAL_EDITOR_FONT_TTF
    // JetBrains Mono as the default font (first face added wins). Size by the
    // window's content scale and shrink FontGlobalScale back so retina renders
    // crisp. fs::exists is mandatory — AddFontFromFileTTF asserts on a missing
    // file rather than returning null.
    {
        ImGuiIO& io = ImGui::GetIO();
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(m_window.handle(), &sx, &sy);
        const float scale = sx > 0.0f ? sx : 1.0f;
        if (fs::exists(LIMINAL_EDITOR_FONT_TTF) &&
            io.Fonts->AddFontFromFileTTF(LIMINAL_EDITOR_FONT_TTF, 16.0f * scale))
            io.FontGlobalScale = 1.0f / scale;
        else
            log("[editor] JetBrains Mono not found, using default font");
    }
#endif

    registerBuiltinComponents();
    m_gizmoOp = ImGuizmo::TRANSLATE;
    log("[editor] liminal editor started");
    if (!projectFile.empty()) openProject(projectFile);
}

// --- loop --------------------------------------------------------------------

void EditorApp::run() {
    double last = m_window.time();
    while (!m_window.shouldClose()) {
        m_window.pollEvents();
        const double now = m_window.time();
        m_dt = std::min(float(now - last), 0.1f);
        last = now;

        m_imgui.beginFrame();
        ImGuizmo::BeginFrame();

#if defined(LIMINAL_WITH_SCRIPTING)
        if (m_mode == Mode::Play && !m_paused && m_scripts)
            m_scripts->update(m_scene, m_dt);
#endif

        // Render the scene into the renderer's low-res FBO. endFrame's blit to
        // the backbuffer is fully covered by the dockspace; the viewport panel
        // shows colorTexture() instead.
        m_view = currentView();
        m_proj = m_renderer.projection();
        m_renderer.beginFrame(m_view);
        renderScene();
        int fbw = 0, fbh = 0;
        m_window.framebufferSize(fbw, fbh);
        m_renderer.endFrame(fbw, fbh);

        drawUi();

        m_imgui.endFrame();
        m_window.swapBuffers();
    }
}

glm::mat4 EditorApp::currentView() {
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
        [&](Entity, Transform& t, MeshRenderer& mr) {
            const Mesh* mesh = m_assets.mesh(mr.meshAsset);
            if (!mesh) return; // unresolved asset: skip, never crash
            DrawItem item;
            item.mesh = mesh;
            item.model = t.matrix();
            item.color = glm::vec3(mr.color);
            item.color2 = glm::vec3(mr.color);
            if (!mr.textureAsset.empty())
                item.texture = m_assets.texture(mr.textureAsset);
            m_renderer.draw(item);
        });
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

    drawMenuBar();
    ImGui::End();

    drawHierarchy();
    drawInspector();
    drawViewport();
    drawAssetBrowser();
    drawConsole();
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

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Asset Browser", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockId);
}

void EditorApp::drawMenuBar() {
    bool wantOpenProject = false, wantOpenScene = false, wantSaveAs = false;

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
            if (ImGui::MenuItem("Quit")) m_window.requestClose();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Game")) {
            if (ImGui::MenuItem("Play", nullptr, false, m_mode == Mode::Edit))
                startPlay();
            if (ImGui::MenuItem("Pause", nullptr, m_paused, m_mode == Mode::Play))
                m_paused = !m_paused;
            if (ImGui::MenuItem("Stop", nullptr, false, m_mode == Mode::Play))
                stopPlay();
            ImGui::EndMenu();
        }
        ImGui::TextDisabled("| %s%s",
                            m_scenePath.empty() ? "(unsaved scene)"
                                                : m_scenePath.c_str(),
                            m_mode == Mode::Play ? "  [PLAY]" : "");
        ImGui::EndMenuBar();
    }

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
}

void EditorApp::drawHierarchy() {
    ImGui::Begin("Hierarchy");

    if (ImGui::Button("+ Empty")) {
        Entity e = m_scene.create("entity");
        e.add<Transform>({});
        m_selected = e.handle();
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Box")) {
        Entity e = m_scene.create("box");
        e.add<Transform>({});
        e.add<MeshRenderer>({});
        m_selected = e.handle();
    }

    entt::registry& reg = m_scene.registry();
    const bool hasSel = m_selected != entt::null && reg.valid(m_selected);
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button("Duplicate")) {
        Entity dup = duplicateEntity(m_selected);
        m_selected = dup.handle();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
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

void EditorApp::drawInspector() {
    ImGui::Begin("Inspector");
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
        if (!keep) ops.removeFrom(reg, m_selected); // header close button = remove
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::BeginCombo("##addcomp", "Add component...")) {
        for (const auto& ops : allOps) {
            if (ops.has(reg, m_selected)) continue;
            if (ImGui::Selectable(ops.name.c_str())) {
                // Empty object -> every field takes its default (built-in
                // fromJson uses json::value with defaults throughout).
                ops.fromJson(reg, m_selected, nlohmann::json::object());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::End();
}

void EditorApp::drawViewport() {
    ImGui::Begin("Viewport");

    // Toolbar: play controls + gizmo mode.
    if (m_mode == Mode::Edit) {
        if (ImGui::Button("Play")) startPlay();
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
    ImVec2 cursor = ImGui::GetCursorPos();
    cursor.x += (avail.x - size.x) * 0.5f;
    cursor.y += (avail.y - size.y) * 0.5f;
    ImGui::SetCursorPos(cursor);

    const ImVec2 imgMin = ImGui::GetCursorScreenPos();
    // FBO texture is bottom-up; flip V.
    ImGui::Image(ImTextureID(uintptr_t(m_renderer.colorTexture())), size,
                 ImVec2(0, 1), ImVec2(1, 0));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    handleCameraInput(hovered);

    // Gizmo mode hotkeys — never while flying or typing.
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && !io.WantTextInput && !m_window.cursorCaptured()) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmoOp = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOp = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOp = ImGuizmo::SCALE;
    }

    const bool gizmoDrawn =
        drawGizmo(glm::vec2(imgMin.x, imgMin.y), glm::vec2(size.x, size.y));

    if (clicked && (!gizmoDrawn || (!ImGuizmo::IsOver() && !ImGuizmo::IsUsing()))) {
        const ImVec2 mouse = ImGui::GetMousePos();
        pickEntity(glm::vec2((mouse.x - imgMin.x) / size.x,
                             (mouse.y - imgMin.y) / size.y));
    }

    ImGui::End();
}

void EditorApp::handleCameraInput(bool viewportHovered) {
    const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (viewportHovered && rmb && !m_window.cursorCaptured())
        m_window.setCursorCaptured(true);
    if (!rmb && m_window.cursorCaptured()) m_window.setCursorCaptured(false);
    if (!m_window.cursorCaptured()) return;

    float dx = 0, dy = 0;
    m_window.mouseDelta(dx, dy);
    m_camYaw -= dx * 0.18f;
    m_camPitch = std::clamp(m_camPitch - dy * 0.18f, -89.0f, 89.0f);

    const float scroll = m_window.scrollDelta();
    if (scroll != 0.0f)
        m_camSpeed = std::clamp(m_camSpeed * (1.0f + scroll * 0.12f), 0.5f, 60.0f);

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

    entt::registry& reg = m_scene.registry();
    entt::entity best = entt::null;
    float bestAlong = 1e9f;
    m_scene.each<Transform>([&](Entity e, Transform& t) {
        // Ray-vs-AABB slab test in entity local space when a mesh is known.
        const Mesh* mesh = nullptr;
        if (const auto* mr = reg.try_get<MeshRenderer>(e.handle()))
            mesh = m_assets.mesh(mr->meshAsset);
        if (mesh) {
            const glm::mat4 invModel = glm::inverse(t.matrix());
            const glm::vec3 lo(invModel * glm::vec4(origin, 1.0f));
            const glm::vec3 ld(invModel * glm::vec4(dir, 0.0f));
            float tMin = 0.0f, tMax = 1e9f;
            bool hit = true;
            for (int i = 0; i < 3; ++i) {
                if (std::abs(ld[i]) < 1e-8f) {
                    if (lo[i] < mesh->localMin[i] || lo[i] > mesh->localMax[i]) {
                        hit = false;
                        break;
                    }
                    continue;
                }
                float t0 = (mesh->localMin[i] - lo[i]) / ld[i];
                float t1 = (mesh->localMax[i] - lo[i]) / ld[i];
                if (t0 > t1) std::swap(t0, t1);
                tMin = std::max(tMin, t0);
                tMax = std::min(tMax, t1);
                if (tMin > tMax) {
                    hit = false;
                    break;
                }
            }
            if (hit) {
                // tMin is in local units along an unnormalized direction; map
                // back to world distance for cross-entity comparison.
                const glm::vec3 worldHit(
                    t.matrix() * glm::vec4(lo + ld * tMin, 1.0f));
                const float along = glm::dot(worldHit - origin, dir);
                if (along >= 0.0f && along < bestAlong) {
                    bestAlong = along;
                    best = e.handle();
                }
            }
            return;
        }
        // Fallback: sphere proxy for entities without a resolvable mesh.
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

void EditorApp::drawAssetBrowser() {
    ImGui::Begin("Asset Browser");

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
        if (ImGui::Selectable(e.name.c_str())) {
            const fs::path p(e.path);
            if (p.extension() == ".lscene") {
                openScene(e.path);
            } else if (p.extension() == ".lua") {
                m_browserStatus = "script: " + e.path;
                log("[editor] script selected: " + e.path +
                    " (assign via a Script component's path field)");
            } else {
                m_browserStatus = e.path;
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

void EditorApp::drawConsole() {
    ImGui::Begin("Console");
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
    refreshAssetTree();
    log("[editor] project open: " + m_projectFile + " (asset root " +
        m_assetRoot + ")");

    const std::string startup = j.value("startupScene", "");
    if (!startup.empty()) openScene(startup);
}

void EditorApp::newScene() {
    if (m_mode == Mode::Play) stopPlay();
    m_scene = Scene();
    m_scenePath.clear();
    m_selected = entt::null;
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
    m_mode = Mode::Play;
    m_paused = false;
#if defined(LIMINAL_WITH_SCRIPTING)
    // Fresh host per play session: clean Lua state, on_start re-runs,
    // parked-error memory wiped.
    m_scripts = std::make_unique<ScriptHost>(&m_window);
    m_scripts->setErrorSink(
        [this](const std::string& msg) { log("[lua] " + msg); });
#endif
    log("[editor] play");
}

void EditorApp::stopPlay() {
    if (m_mode != Mode::Play) return;
#if defined(LIMINAL_WITH_SCRIPTING)
    m_scripts.reset();
#endif
    m_scene = Scene();
    sceneFromJson(m_scene, m_playSnapshot);
    m_playSnapshot = nullptr;
    m_mode = Mode::Edit;
    m_paused = false;
    m_selected = entt::null; // entity ids changed on restore
    log("[editor] stop — scene restored from snapshot");
}

void EditorApp::log(const std::string& line) {
    m_console.push_back(line);
    if (m_console.size() > 2000)
        m_console.erase(m_console.begin(),
                        m_console.begin() + ptrdiff_t(m_console.size() - 1000));
    m_consoleScrollDown = true;
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
