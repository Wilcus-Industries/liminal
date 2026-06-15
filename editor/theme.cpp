#include "theme.hpp"

#include <imgui.h>

namespace liminal::editor::theme {

namespace {

// --- "Dark" — the editor's historical look (ImGui's stock dark). -------------
void applyDark(ImGuiStyle& s) {
    ImGui::StyleColorsDark(&s);
}

// --- "Light" — ImGui's stock light. ------------------------------------------
void applyLight(ImGuiStyle& s) {
    ImGui::StyleColorsLight(&s);
}

// --- "Liminal" — dark, desaturated blue-grey with rounded, roomy widgets. ----
void applyLiminal(ImGuiStyle& s) {
    ImGui::StyleColorsDark(&s);

    s.WindowRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;

    ImVec4* c = s.Colors;
    const ImVec4 bg = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    const ImVec4 panel = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    const ImVec4 accent = ImVec4(0.30f, 0.52f, 0.74f, 1.00f);
    const ImVec4 accentHi = ImVec4(0.38f, 0.62f, 0.86f, 1.00f);

    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg;
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.98f);
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.27f, 0.32f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_TitleBgActive] = panel;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderHovered] = accent;
    c[ImGuiCol_HeaderActive] = accentHi;
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = accent;
    c[ImGuiCol_ButtonActive] = accentHi;
    c[ImGuiCol_CheckMark] = accentHi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHi;
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered] = accent;
    c[ImGuiCol_TabActive] = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
    c[ImGuiCol_TabUnfocused] = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
}

// --- "High Contrast" — near-black with bright amber accents for visibility. --
void applyHighContrast(ImGuiStyle& s) {
    ImGui::StyleColorsDark(&s);

    s.WindowRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.TabRounding = 0.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    const ImVec4 bg = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
    const ImVec4 amber = ImVec4(0.95f, 0.62f, 0.06f, 1.00f);
    const ImVec4 amberDim = ImVec4(0.55f, 0.36f, 0.04f, 1.00f);

    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg;
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.96f, 0.96f, 1.00f);
    c[ImGuiCol_Border] = amberDim;
    c[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = amberDim;
    c[ImGuiCol_FrameBgActive] = amber;
    c[ImGuiCol_TitleBgActive] = amberDim;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    c[ImGuiCol_Header] = amberDim;
    c[ImGuiCol_HeaderHovered] = amber;
    c[ImGuiCol_HeaderActive] = amber;
    c[ImGuiCol_Button] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_ButtonHovered] = amberDim;
    c[ImGuiCol_ButtonActive] = amber;
    c[ImGuiCol_CheckMark] = amber;
    c[ImGuiCol_SliderGrab] = amber;
    c[ImGuiCol_SliderGrabActive] = amber;
    c[ImGuiCol_Tab] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_TabHovered] = amberDim;
    c[ImGuiCol_TabActive] = amberDim;
    c[ImGuiCol_Separator] = amberDim;
    c[ImGuiCol_DockingPreview] = ImVec4(amber.x, amber.y, amber.z, 0.55f);
}

} // namespace

const std::vector<Theme>& registry() {
    static const std::vector<Theme> kThemes = {
        {"Dark", &applyDark},
        {"Light", &applyLight},
        {"Liminal", &applyLiminal},
        {"High Contrast", &applyHighContrast},
    };
    return kThemes;
}

const Theme* find(const std::string& name) {
    for (const auto& t : registry())
        if (t.name == name) return &t;
    return nullptr;
}

} // namespace liminal::editor::theme
