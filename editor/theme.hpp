#pragma once

#include <functional>
#include <string>
#include <vector>

struct ImGuiStyle;

// Self-contained ImGui theme registry. Holds no EditorApp dependency so any UI
// surface — the Theme menu today, a settings window later — can list themes and
// apply one through the same data. A Theme's `apply` mutates an ImGuiStyle in
// place: it may seed from an ImGui::StyleColors* base and then override colors
// and layout vars.
namespace liminal::editor::theme {

struct Theme {
    std::string name;
    std::function<void(ImGuiStyle&)> apply;
};

// Built-in themes in stable display order (Dark, Light, Liminal, High Contrast).
// This is display order only — the registry does not pick a default; EditorApp
// chooses the active theme at startup (currently "Liminal", editor_app.hpp).
const std::vector<Theme>& registry();

// Lookup by name; nullptr if unknown.
const Theme* find(const std::string& name);

} // namespace liminal::editor::theme
