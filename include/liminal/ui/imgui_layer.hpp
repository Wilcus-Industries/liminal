#pragma once
// Dear ImGui lifecycle wrapper. The overlay is the one part of the game that
// is allowed to feel snappy — it's a developer tool, not part of the dream.
// Panels themselves are drawn by the game layer between begin/end.

namespace liminal {

class Window;

class ImGuiLayer {
public:
    explicit ImGuiLayer(Window& window);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    void beginFrame(); // ImGui_ImplX_NewFrame + ImGui::NewFrame
    void endFrame();   // ImGui::Render + RenderDrawData

private:
    // False when constructed against a windowless (offscreen/headless) context:
    // ImGui's GLFW backend needs a real GLFWwindow, so the whole layer becomes a
    // no-op shell. runHeadless never draws UI anyway.
    bool m_active = false;
};

} // namespace liminal
