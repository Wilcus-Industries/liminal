#pragma once
// ScriptEditorPanel: the "Script Editor" dock window. A tab bar of open text
// files, each backed by its own ImGuiColorTextEdit (BalazsJako) instance with
// a dirty flag, plus (for Lua files only) an as-you-type completion popup and
// (when scripting is compiled in) compile-only diagnostics. Non-Lua text files
// open with extension-based highlighting but no completion/diagnostics.
//
// It is owned by EditorApp but coupled to it only through a std::function log
// sink — it never reaches back into the editor. Saving from here writes the
// tab's file straight to disk; ScriptHost's 0.5s mtime watch picks that up and
// hot-reloads during Play with no extra wiring (see save()).

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h> // ImVec2 for the popup anchor

class TextEditor; // ImGuiColorTextEdit, fwd-declared to keep the header light

namespace liminal::editor {

class ScriptEditorPanel {
public:
    using LogSink = std::function<void(const std::string&)>;

    explicit ScriptEditorPanel(LogSink log);
    ~ScriptEditorPanel(); // out-of-line: TextEditor is incomplete here

    // Load `path` into a tab (switching to it if already open) and focus the
    // pane next frame.
    void open(const std::string& path);

    // Draw the dock window. Call once per frame from EditorApp::drawUi.
    void draw(float dt);

    // True while the Script Editor window (or a child of it) had focus on the
    // last draw — EditorApp uses this to route the global Cmd/Ctrl+S.
    bool focused() const { return m_focused; }

    // True if any open tab holds unsaved edits — EditorApp uses this to gate
    // Close Project behind a discard-confirm modal.
    bool anyDirty() const;

private:
    struct Doc {
        std::string path;                  // absolute file path
        std::unique_ptr<TextEditor> editor;
        bool isLua = false;                // gates completion + diagnostics
        bool dirty = false;
        bool wantClose = false;            // close requested; may need confirm
        std::string lastText;              // for change detection / debounce
        float diagTimer = 0.0f;            // debounce accumulator (seconds)
        bool diagDirty = false;            // text changed since last lint
        int diagLine = 0;                  // 0 = clean, else 1-based error line
        std::string diagMessage;           // last syntax-error message
        std::filesystem::file_time_type lastWriteTime{}; // mtime we last saw on
                                           // disk; stamped on open/save so our
                                           // own writes never self-trigger a
                                           // reload
    };

    void drawDoc(Doc& doc, float dt);
    void closeDoc(int i); // erase + reindex active/popup/confirm indices
    void save(Doc& doc);
    // Read a file, applying the binary sniff + size cap; returns false (and
    // logs) if rejected/unreadable. Shared by open() and the disk reloader.
    bool readFile(const std::string& abs, std::string& out);
    void checkDiskChanges(float dt);       // throttled external-edit detector
    bool saveActive();                     // returns false if no active tab
    void runDiagnostics(Doc& doc);
    void drawCompletionPopup(Doc& doc);
    void dismissPopup();

    std::vector<Doc> m_docs;
    int m_active = -1;        // index into m_docs, -1 = none
    LogSink m_log;
    bool m_wantFocus = false; // set by open(), consumed in draw()
    bool m_focused = false;   // window focus captured each draw()

    // close-confirm modal target (index captured when the modal opens)
    int m_confirmClose = -1;
    // reload-conflict modal target: doc whose disk copy changed while its
    // buffer holds unsaved edits (-1 = no modal)
    int m_confirmReload = -1;
    // throttle accumulator for the disk-change poll (~0.5s cadence)
    float m_diskCheckTimer = 0.0f;

    // --- completion popup state (one popup, for the active doc) ---
    bool m_popupOpen = false;
    int m_popupDoc = -1;       // which doc the popup belongs to
    int m_popupSel = 0;        // highlighted item
    std::string m_popupPrefix; // partial word the items replace
    // Editor child origin + scroll captured during Render, reused to anchor
    // the popup under the text cursor.
    ImVec2 m_lastEditorOrigin{0.0f, 0.0f};
    ImVec2 m_lastEditorScroll{0.0f, 0.0f};
    // items are recomputed each frame the popup is open; see draw.
};

} // namespace liminal::editor
