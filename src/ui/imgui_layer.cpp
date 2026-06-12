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
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

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
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame() {
    // Render() finalizes the draw lists; RenderDrawData replays them with
    // the GL3 backend's own state (it saves/restores GL state itself, so it
    // won't trample the renderer's bindings).
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace liminal
