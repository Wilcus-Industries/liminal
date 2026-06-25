#include "edit_history.hpp"

#include <liminal/scene/scene.hpp>
#include <liminal/scene/serialize.hpp>

namespace liminal::editor {

void EditHistory::capDepth(std::vector<nlohmann::json>& stack) {
    if (stack.size() > kMaxDepth)
        stack.erase(stack.begin(),
                    stack.begin() +
                        std::ptrdiff_t(stack.size() - kMaxDepth));
}

void EditHistory::pushUndo(nlohmann::json snap) {
    m_undo.push_back(std::move(snap));
    capDepth(m_undo);
    m_redo.clear();
}

void EditHistory::pushSnapshot(const Scene& scene) {
    pushUndo(sceneToJson(scene));
    // The discrete mutation lands this frame; refresh the clean baseline so a
    // following continuous interaction diffs against the post-mutation state.
    m_clean.reset();
}

void EditHistory::tick(const Scene& scene, bool interacting) {
    if (!interacting) {
        if (m_wasInteracting && m_clean) {
            // Interaction just ended: commit the pre-drag baseline if the scene
            // actually changed during the drag.
            nlohmann::json now = sceneToJson(scene);
            if (now != *m_clean) pushUndo(std::move(*m_clean));
            m_clean = std::move(now);
        } else {
            // Idle frame: keep the baseline current so the next interaction's
            // undo entry is the true pre-edit state.
            m_clean = sceneToJson(scene);
        }
    }
    m_wasInteracting = interacting;
}

bool EditHistory::undo(Scene& scene) {
    if (m_undo.empty()) return false;
    m_redo.push_back(sceneToJson(scene));
    capDepth(m_redo);
    nlohmann::json snap = std::move(m_undo.back());
    m_undo.pop_back();
    scene = Scene();
    sceneFromJson(scene, snap);
    m_clean = std::move(snap);
    m_wasInteracting = false;
    return true;
}

bool EditHistory::redo(Scene& scene) {
    if (m_redo.empty()) return false;
    m_undo.push_back(sceneToJson(scene));
    capDepth(m_undo);
    nlohmann::json snap = std::move(m_redo.back());
    m_redo.pop_back();
    scene = Scene();
    sceneFromJson(scene, snap);
    m_clean = std::move(snap);
    m_wasInteracting = false;
    return true;
}

void EditHistory::clear() {
    m_undo.clear();
    m_redo.clear();
    m_clean.reset();
    m_wasInteracting = false;
}

} // namespace liminal::editor
