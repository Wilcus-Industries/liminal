// ImGuiLayer.cpp — Dear ImGui lifecycle, nothing more. Panel contents are
// drawn by the game layer between beginFrame()/endFrame(); this class only
// owns the context and the GLFW/OpenGL3 backend pair.

#include <liminal/ui/imgui_layer.hpp>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <liminal/core/window.hpp>

namespace liminal {

ImGuiLayer::ImGuiLayer(Window& window) {
    // The ImGui context itself (io flags, style, font atlas) is CPU-side and is
    // always created — the editor ctor touches ImGui::GetIO()/fonts even in
    // headless. Only the GLFW + OpenGL3 BACKENDS need a real GLFWwindow + a
    // presentable context, so they're skipped for a windowless (offscreen)
    // context; with no backends, beginFrame/endFrame become no-ops and no UI is
    // ever drawn (runHeadless never calls them anyway).
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (window.handle() == nullptr) return; // offscreen: context only, no backends
    m_active = true;

    // install_callbacks = true: ImGui chains GLFW's input callbacks and
    // forwards to any previously-installed ones, so Window's own input
    // handling keeps working underneath the overlay.
    ImGui_ImplGlfw_InitForOpenGL(window.handle(), true);
    // The GLSL version string must match what this context can actually
    // compile — 4.1 core is the macOS ceiling, hence "#version 410".
    ImGui_ImplOpenGL3_Init("#version 410");
}

ImGuiLayer::~ImGuiLayer() {
    // Backends must come down before the context they registered with.
    if (m_active) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
    ImGui::DestroyContext();
}

void ImGuiLayer::beginFrame() {
    if (!m_active) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame() {
    if (!m_active) return;
    // Render() finalizes the draw lists; RenderDrawData replays them with
    // the GL3 backend's own state (it saves/restores GL state itself, so it
    // won't trample the renderer's bindings).
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace liminal
