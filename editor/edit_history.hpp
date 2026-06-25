#pragma once
// EditHistory: undo/redo for editor scene edits (Edit mode only). Stores whole-
// scene JSON snapshots (sceneToJson/sceneFromJson — the same infra the Play
// snapshot uses) on two bounded stacks. Scenes are small, so a snapshot stack is
// simpler and more robust than per-edit command objects.
//
// Two ways an edit enters the history:
//   * Discrete mutations (create / delete / duplicate / add+remove component /
//     MCP tool calls): the caller calls pushSnapshot(scene) BEFORE mutating.
//   * Continuous widget edits (gizmo drag, inspector DragFloat3): the editor
//     calls tick(scene, interacting) once per frame; a whole drag coalesces into
//     a single undo entry (the pre-drag "clean" state is what gets pushed).
//
// Entity entt-ids do not survive sceneFromJson, so undo/redo return the Name of
// the previously selected entity for the caller to re-resolve selection.

#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

namespace liminal {
class Scene;
}

namespace liminal::editor {

class EditHistory {
public:
    // Snapshot the current scene onto the undo stack and drop the redo stack.
    // Call BEFORE a discrete mutation so the snapshot is the pre-edit state.
    void pushSnapshot(const Scene& scene);

    // Per-frame coalescing of continuous widget interactions. `interacting` is
    // true while any ImGui item is active or ImGuizmo is in use. On the release
    // edge, if the scene actually changed since the last idle frame, the pre-
    // interaction snapshot is committed as one undo entry.
    void tick(const Scene& scene, bool interacting);

    // Restore the top undo (resp. redo) snapshot, pushing the current state onto
    // the opposite stack. Returns false when the stack is empty. selName is set
    // to the Name of the entity that was selected (caller passes the current
    // selection's Name in, gets the restored target back out) — used to re-
    // resolve selection after entt-ids are reset by the load.
    bool undo(Scene& scene);
    bool redo(Scene& scene);

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    // Wipe all history (scene/project load, Play start/stop).
    void clear();

private:
    static constexpr std::size_t kMaxDepth = 100;

    void pushUndo(nlohmann::json snap); // bounded push, clears redo
    static void capDepth(std::vector<nlohmann::json>& stack);

    std::vector<nlohmann::json> m_undo;
    std::vector<nlohmann::json> m_redo;
    // Last scene state observed on a non-interacting frame; the pre-drag baseline
    // committed when a continuous interaction ends having changed something.
    std::optional<nlohmann::json> m_clean;
    bool m_wasInteracting = false;
};

} // namespace liminal::editor
