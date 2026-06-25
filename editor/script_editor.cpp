// ScriptEditorPanel implementation. Tabs of TextEditor instances; an as-you-
// type completion popup that steals navigation keys from the widget by
// flipping SetHandleKeyboardInputs(false) for the frame; and compile-only Lua
// diagnostics surfaced as error markers + a status bar (scripting builds only).

#include "script_editor.hpp"
#include "lua_complete.hpp"

#include <TextEditor.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName: query the widget child's scroll

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace fs = std::filesystem;

namespace liminal::editor {

namespace {

// Filename for a tab label (full path is shown as a tooltip).
std::string fileName(const std::string& path) {
    return fs::path(path).filename().string();
}

// TextEditor's Coordinates::mColumn is a display column (tabs expand to the
// next tabSize stop, a UTF-8 sequence is one column), not a byte index. Walk
// the line the same way GetCharacterIndex does (private upstream) to get the
// byte offset the cursor sits at.
std::size_t columnToByteOffset(const std::string& line, int column, int tabSize) {
    int col = 0;
    std::size_t i = 0;
    while (i < line.size() && col < column) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c == '\t') {
            col = (col / tabSize) * tabSize + tabSize;
            ++i;
        } else {
            ++col;
            if (c < 0x80) i += 1;
            else if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else i += 4;
        }
    }
    return std::min(i, line.size());
}

// git's binary heuristic: a NUL byte in the leading bytes means binary. Cheap
// and good enough to keep the TextEditor from choking on arbitrary content.
bool looksLikeText(const std::string& bytes) {
    const std::size_t n = std::min<std::size_t>(bytes.size(), 8000);
    for (std::size_t i = 0; i < n; ++i)
        if (bytes[i] == '\0') return false;
    return true;
}

// Pick a syntax-highlight language from a lowercased extension. nullptr means
// plain text (colorizer disabled). Only Lua wires up completion/diagnostics
// elsewhere; the rest are highlight-only.
const TextEditor::LanguageDefinition* languageFor(const std::string& extLower) {
    if (extLower == ".lua") return &TextEditor::LanguageDefinition::Lua();
    if (extLower == ".c" || extLower == ".h")
        return &TextEditor::LanguageDefinition::C();
    if (extLower == ".cpp" || extLower == ".cc" || extLower == ".cxx" ||
        extLower == ".hpp" || extLower == ".hh" || extLower == ".inl")
        return &TextEditor::LanguageDefinition::CPlusPlus();
    if (extLower == ".glsl" || extLower == ".vert" || extLower == ".frag" ||
        extLower == ".geom" || extLower == ".comp" || extLower == ".tesc" ||
        extLower == ".tese")
        return &TextEditor::LanguageDefinition::GLSL();
    if (extLower == ".hlsl" || extLower == ".fx")
        return &TextEditor::LanguageDefinition::HLSL();
    if (extLower == ".sql") return &TextEditor::LanguageDefinition::SQL();
    if (extLower == ".as") return &TextEditor::LanguageDefinition::AngelScript();
    return nullptr;
}

} // namespace

ScriptEditorPanel::ScriptEditorPanel(LogSink log) : m_log(std::move(log)) {}

ScriptEditorPanel::~ScriptEditorPanel() = default; // TextEditor complete here

bool ScriptEditorPanel::readFile(const std::string& abs, std::string& out) {
    // Size cap: TextEditor stores a per-character glyph, so a large file chokes
    // it. Reject before reading.
    std::error_code ec;
    auto sz = fs::file_size(abs, ec);
    if (!ec && sz > 2 * 1024 * 1024) {
        if (m_log) m_log("[script] file too large: " + abs);
        return false;
    }

    std::ifstream in(abs, std::ios::binary);
    if (!in) {
        if (m_log) m_log("[script] cannot open: " + abs);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();

    // Content sniff is the authoritative binary guard for unknown extensions.
    if (!looksLikeText(out)) {
        if (m_log) m_log("[script] not a text file: " + abs);
        return false;
    }
    return true;
}

void ScriptEditorPanel::open(const std::string& path) {
    const std::string abs = fs::absolute(path).lexically_normal().string();

    // Already open? switch to it.
    for (int i = 0; i < int(m_docs.size()); ++i) {
        if (m_docs[i].path == abs) {
            m_active = i;
            m_wantFocus = true;
            return;
        }
    }

    std::string contents;
    if (!readFile(abs, contents)) return; // logs its own rejection reason

    std::string ext = fs::path(abs).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });

    Doc doc;
    doc.path = abs;
    doc.isLua = (ext == ".lua");
    doc.editor = std::make_unique<TextEditor>();
    if (const TextEditor::LanguageDefinition* lang = languageFor(ext)) {
        TextEditor::LanguageDefinition def = *lang;
        def.mAutoIndentation = true; // Lua/AngelScript defs ship with this off
        doc.editor->SetLanguageDefinition(def);
    } else
        doc.editor->SetColorizerEnable(false); // plain text, no tokenizer
    doc.editor->SetShowWhitespaces(false);
    doc.editor->SetText(contents);
    doc.lastText = doc.editor->GetText();
    doc.diagDirty = doc.isLua; // only Lua lints, and once on open
    // Stamp the on-disk mtime so the reload poll only fires on external edits.
    std::error_code wec;
    doc.lastWriteTime = fs::last_write_time(abs, wec);
    m_docs.push_back(std::move(doc));
    m_active = int(m_docs.size()) - 1;
    m_wantFocus = true;
    if (m_log) m_log("[script] opened: " + abs);
}

void ScriptEditorPanel::save(Doc& doc) {
    // Plain write to the tab's path. ScriptHost watches file mtimes on a 0.5s
    // cadence during Play, so this save hot-reloads the running script with no
    // further wiring on our side. Note: the vendored TextEditor::SetText drops
    // every '\r', so a round-trip normalizes line endings to LF — accepted.
    std::ofstream out(doc.path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (m_log) m_log("[script] save FAILED: " + doc.path);
        return;
    }
    const std::string text = doc.editor->GetText();
    out << text;
    out.close();
    doc.dirty = false;
    doc.lastText = text;
    // Restamp mtime AFTER the write so the reload poll treats this as our own
    // change, not an external edit.
    std::error_code wec;
    doc.lastWriteTime = fs::last_write_time(doc.path, wec);
    if (m_log) m_log("[script] saved: " + doc.path);
}

bool ScriptEditorPanel::saveActive() {
    if (m_active < 0 || m_active >= int(m_docs.size())) return false;
    save(m_docs[size_t(m_active)]);
    return true;
}

bool ScriptEditorPanel::anyDirty() const {
    for (const Doc& doc : m_docs)
        if (doc.dirty) return true;
    return false;
}

void ScriptEditorPanel::runDiagnostics(Doc& doc) {
    TextEditor::ErrorMarkers markers;
    std::string status;

    lua_State* L = luaL_newstate(); // throwaway: compile only, never execute
    if (L) {
        const std::string text = doc.editor->GetText();
        // Compile-only: a syntax error leaves a "<name>:<line>: <message>"
        // string on the stack and returns non-zero. The leading '=' makes the
        // chunk name a literal "s" (not the source text), so the error reads
        // "s:<line>: <message>" — easy and unambiguous to parse.
        if (luaL_loadbuffer(L, text.c_str(), text.size(), "=s") != LUA_OK) {
            const char* msg = lua_tostring(L, -1);
            std::string m = msg ? msg : "syntax error";
            int line = 0;
            std::string message = m;
            // Strip a leading "s:<digits>: " prefix.
            if (m.rfind("s:", 0) == 0) {
                const std::size_t c = m.find(':', 2);
                if (c != std::string::npos) {
                    const std::string num = m.substr(2, c - 2);
                    bool allDigits = !num.empty();
                    for (char ch : num)
                        if (ch < '0' || ch > '9') allDigits = false;
                    if (allDigits) {
                        line = std::atoi(num.c_str());
                        message = m.substr(c + 1);
                        while (!message.empty() && message.front() == ' ')
                            message.erase(message.begin());
                    }
                }
            }
            if (line > 0) markers[line] = message;
            status = "error: " + message +
                     (line > 0 ? " (line " + std::to_string(line) + ")" : "");
        }
        lua_close(L);
    }

    doc.editor->SetErrorMarkers(markers);
    if (markers.empty()) {
        doc.diagLine = 0;
        doc.diagMessage.clear();
    } else {
        doc.diagLine = markers.begin()->first;
        doc.diagMessage = markers.begin()->second;
    }
    (void)status;
}

void ScriptEditorPanel::closeDoc(int i) {
    // Indices into m_docs shift down past the erase point; keep the active
    // tab, the popup's doc, and a pending confirm-close pointing at the same
    // documents they did before.
    if (m_popupDoc == i)
        dismissPopup();
    else if (m_popupDoc > i)
        --m_popupDoc;
    m_docs.erase(m_docs.begin() + i);
    if (m_active > i) --m_active;
    if (m_active >= int(m_docs.size())) m_active = int(m_docs.size()) - 1;
    if (m_confirmClose > i) --m_confirmClose;
    if (m_confirmReload == i)
        m_confirmReload = -1; // the conflicted doc is gone
    else if (m_confirmReload > i)
        --m_confirmReload;
}

void ScriptEditorPanel::dismissPopup() {
    m_popupOpen = false;
    m_popupDoc = -1;
    m_popupSel = 0;
    m_popupPrefix.clear();
}

void ScriptEditorPanel::draw(float dt, const char* windowTitle, bool* pOpen) {
    if (m_wantFocus) ImGui::SetNextWindowFocus();
    ImGui::Begin(windowTitle, pOpen);
    // ImGui only focuses windows on left-click; focus on right-click too.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsWindowFocused()) ImGui::SetWindowFocus();
    m_wantFocus = false;
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (m_docs.empty()) {
        ImGui::TextDisabled("no file open");
        ImGui::TextDisabled("double-click a file in the Asset Browser");
        ImGui::End();
        return;
    }

    // Detect external edits (e.g. an AI tool rewriting the file) and reload or
    // raise a conflict modal. Throttled inside; cheap to call each frame.
    checkDiskChanges(dt);

    // Toolbar.
    if (ImGui::Button("Save")) saveActive();
    ImGui::SameLine();
    const bool activeLua = m_active >= 0 && m_active < int(m_docs.size()) &&
                           m_docs[size_t(m_active)].isLua;
    if (activeLua)
        ImGui::TextDisabled("Cmd/Ctrl+S saves  •  Ctrl+Space completes");
    else
        ImGui::TextDisabled("Cmd/Ctrl+S saves");
    ImGui::Separator();

    // No Reorderable: tab IDs are positional (PushID(i) + "###tab"), so a
    // visual reorder could not move the backing Doc and would snap back.
    if (ImGui::BeginTabBar("##scripttabs", ImGuiTabBarFlags_AutoSelectNewTabs)) {
        for (int i = 0; i < int(m_docs.size()); ++i) {
            Doc& doc = m_docs[size_t(i)];
            ImGui::PushID(i);
            bool open = true;
            std::string label =
                (doc.dirty ? "* " : "") + fileName(doc.path) + "###tab";
            ImGuiTabItemFlags flags =
                doc.dirty ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                m_active = i;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", doc.path.c_str());
                drawDoc(doc, dt);
                ImGui::EndTabItem();
            }
            if (!open) doc.wantClose = true;
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }

    // Resolve close requests one per frame: dirty tabs route through a
    // confirm modal, clean ones close immediately. The break keeps a second
    // same-frame request (its wantClose still set) alive for next frame
    // instead of overwriting the modal target.
    for (int i = 0; i < int(m_docs.size()); ++i) {
        if (!m_docs[size_t(i)].wantClose) continue;
        if (m_docs[size_t(i)].dirty) {
            m_confirmClose = i;
            m_docs[size_t(i)].wantClose = false;
            ImGui::OpenPopup("Discard changes?");
        } else {
            closeDoc(i);
        }
        break;
    }

    if (ImGui::BeginPopupModal("Discard changes?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_confirmClose >= 0 && m_confirmClose < int(m_docs.size())) {
            ImGui::Text("\"%s\" has unsaved changes.",
                        fileName(m_docs[size_t(m_confirmClose)].path).c_str());
            if (ImGui::Button("Save & Close")) {
                save(m_docs[size_t(m_confirmClose)]);
                closeDoc(m_confirmClose);
                m_confirmClose = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard")) {
                closeDoc(m_confirmClose);
                m_confirmClose = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                m_confirmClose = -1;
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Reload-conflict modal: the file changed on disk while the buffer holds
    // unsaved edits. "Keep mine" restamps the mtime so we stop nagging until
    // the next external change; "Load disk" reloads and clears dirty.
    if (ImGui::BeginPopupModal("File changed on disk", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_confirmReload >= 0 && m_confirmReload < int(m_docs.size())) {
            Doc& doc = m_docs[size_t(m_confirmReload)];
            ImGui::Text("\"%s\" changed on disk but has unsaved edits.",
                        fileName(doc.path).c_str());
            if (ImGui::Button("Keep mine")) {
                // Stop nagging until the file changes again.
                std::error_code wec;
                doc.lastWriteTime = fs::last_write_time(doc.path, wec);
                m_confirmReload = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Load disk")) {
                std::string contents;
                if (readFile(doc.path, contents)) {
                    doc.editor->SetText(contents);
                    doc.lastText = doc.editor->GetText();
                    doc.dirty = false;
                    doc.diagDirty = doc.isLua;
                    std::error_code wec;
                    doc.lastWriteTime = fs::last_write_time(doc.path, wec);
                    if (m_log)
                        m_log("[script] reloaded from disk: " + doc.path);
                }
                m_confirmReload = -1;
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_active >= int(m_docs.size())) m_active = int(m_docs.size()) - 1;
    ImGui::End();
}

void ScriptEditorPanel::checkDiskChanges(float dt) {
    // Throttle: stat'ing every open file each frame is wasteful, and external
    // tools don't change files faster than a human notices.
    m_diskCheckTimer += dt;
    if (m_diskCheckTimer < 0.5f) return;
    m_diskCheckTimer = 0.0f;

    // One conflict modal at a time; if one is pending, leave the rest until it
    // resolves.
    if (m_confirmReload >= 0) return;

    for (int i = 0; i < int(m_docs.size()); ++i) {
        Doc& doc = m_docs[size_t(i)];
        std::error_code ec;
        const auto mtime = fs::last_write_time(doc.path, ec);
        if (ec) continue; // file vanished or unreadable — leave the buffer as-is
        if (mtime <= doc.lastWriteTime) continue; // unchanged (or our own write)

        if (doc.dirty) {
            // Unsaved local edits: never clobber silently. Raise the modal and
            // stop scanning so we only juggle one conflict at a time.
            m_confirmReload = i;
            ImGui::OpenPopup("File changed on disk");
            break;
        }

        // Clean buffer: reload in place, preserving cursor + scroll where the
        // file is long enough to still hold them.
        std::string contents;
        if (!readFile(doc.path, contents)) {
            // Became binary/too large/unreadable since open: restamp so we
            // don't re-trigger every poll.
            doc.lastWriteTime = mtime;
            continue;
        }
        const TextEditor::Coordinates cursor = doc.editor->GetCursorPosition();
        doc.editor->SetText(contents);
        doc.editor->SetCursorPosition(cursor); // clamped internally if past EOF
        doc.lastText = doc.editor->GetText();
        doc.diagDirty = doc.isLua;
        doc.lastWriteTime = mtime;
        if (m_log) m_log("[script] reloaded from disk: " + doc.path);
    }
}

void ScriptEditorPanel::drawDoc(Doc& doc, float dt) {
    const int docIndex = m_active;
    const bool isPopupDoc = m_popupOpen && m_popupDoc == docIndex;

    // While the popup is open, steal ONLY its navigation/accept keys from the
    // widget — and only on frames where one is actually pressed. Ordinary
    // typing must keep flowing into the editor so the popup filters as you
    // type instead of blocking input.
    const bool popupNavKey =
        isPopupDoc && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
                       ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                       ImGui::IsKeyPressed(ImGuiKey_Tab, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    doc.editor->SetHandleKeyboardInputs(!popupNavKey);

    // Reserve room for the status bar at the bottom.
    const float statusH = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 editorSize(avail.x, avail.y - statusH);

    // Capture the editor child's screen origin for popup placement, and
    // compose the child window's full name the way BeginChild does so we can
    // query its scroll after Render — the widget exposes no scroll accessor,
    // and text drifts under the fixed child rect as it scrolls.
    const ImVec2 editorOrigin = ImGui::GetCursorScreenPos();
    char childName[256];
    std::snprintf(childName, sizeof(childName), "%s/%s_%08X",
                  ImGui::GetCurrentWindow()->Name, "##editor",
                  ImGui::GetID("##editor"));
    doc.editor->Render("##editor", editorSize, false);
    if (ImGuiWindow* child = ImGui::FindWindowByName(childName))
        m_lastEditorScroll = ImVec2(child->Scroll.x, child->Scroll.y);
    else
        m_lastEditorScroll = ImVec2(0.0f, 0.0f);

    doc.editor->SetHandleKeyboardInputs(true); // restore default for next frame

    // --- change tracking + dirty flag ---
    if (doc.editor->IsTextChanged()) {
        doc.dirty = true;
        if (doc.isLua) {
            doc.diagDirty = true;
            doc.diagTimer = 0.0f;
            // Trigger the popup as-you-type: if the char left of the cursor is
            // an identifier char or a '.'/':' just typed, (re)open the popup.
            if (m_popupDoc != docIndex || !m_popupOpen) {
                m_popupOpen = true;
                m_popupDoc = docIndex;
                m_popupSel = 0;
            }
        }
    }

    // --- Save shortcut (Cmd on mac / Ctrl elsewhere). Gated on the window
    // focus EditorApp also checks, so exactly one of {script save, scene
    // save} responds to the chord.
    ImGuiIO& io = ImGui::GetIO();
    const bool mod = io.KeySuper || io.KeyCtrl;
    if (m_focused && mod && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        save(doc);
    }

    // --- manual completion trigger: Ctrl+Space (Lua only) ---
    if (doc.isLua && m_focused && io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        m_popupOpen = true;
        m_popupDoc = docIndex;
        m_popupSel = 0;
    }

    // --- debounced diagnostics (~0.5s, frame-time accumulated; Lua only) ---
    if (doc.isLua && doc.diagDirty) {
        doc.diagTimer += dt;
        if (doc.diagTimer >= 0.5f) {
            runDiagnostics(doc);
            doc.diagDirty = false;
            doc.diagTimer = 0.0f;
        }
    }

    m_lastEditorOrigin = editorOrigin;

    // --- completion popup (anchored to the text cursor) ---
    if (m_popupOpen && m_popupDoc == docIndex)
        drawCompletionPopup(doc);

    // --- status bar: Lua docs show diagnostics (or the needs-scripting note);
    // any other text file shows the cursor position. ---
    ImGui::Separator();
    if (doc.isLua) {
        if (doc.diagLine > 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "line %d: %s",
                               doc.diagLine, doc.diagMessage.c_str());
        } else {
            ImGui::TextDisabled("no syntax errors");
        }
    } else {
        TextEditor::Coordinates cp = doc.editor->GetCursorPosition();
        ImGui::TextDisabled("Ln %d, Col %d", cp.mLine + 1, cp.mColumn + 1);
    }
}

// Insert the part of a completion item that extends past the already-typed
// prefix (the whole item on a case-mismatch). Shared by the keyboard-accept
// and mouse-click-accept paths of drawCompletionPopup.
static void insertCompletionRemainder(TextEditor* editor,
                                      const std::string& insert,
                                      const std::string& prefix) {
    std::string remainder;
    if (insert.size() >= prefix.size() &&
        insert.compare(0, prefix.size(), prefix) == 0)
        remainder = insert.substr(prefix.size());
    else
        remainder = insert; // case mismatch: insert whole (rare)
    if (!remainder.empty()) editor->InsertText(remainder);
}

void ScriptEditorPanel::drawCompletionPopup(Doc& doc) {
    // Current line text up to the cursor → context for the completion engine.
    const TextEditor::Coordinates cur = doc.editor->GetCursorPosition();
    const std::string allText = doc.editor->GetText();

    // Pull out the current line's text (cur.mLine is 0-based).
    std::string lineText;
    {
        int line = 0;
        std::size_t i = 0;
        std::size_t lineStart = 0;
        for (; i < allText.size() && line < cur.mLine; ++i)
            if (allText[i] == '\n') { ++line; lineStart = i + 1; }
        std::size_t lineEnd = allText.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = allText.size();
        lineText = allText.substr(lineStart, lineEnd - lineStart);
        // mColumn is a display column, not a byte index — convert before
        // slicing, or any tab/UTF-8 char left of the cursor skews the prefix.
        const std::size_t byteCol = columnToByteOffset(
            lineText, cur.mColumn, doc.editor->GetTabSize());
        if (byteCol < lineText.size()) lineText = lineText.substr(0, byteCol);
    }

    std::string prefix;
    std::vector<CompletionItem> items =
        computeCompletions(lineText, allText, prefix, 10);

    // No matches → close, and only auto-open meaningfully once 1+ ident char
    // or a separator is present. A bare empty prefix with no separator means
    // we're between tokens: keep quiet.
    const bool afterSep = !lineText.empty() &&
                          (lineText.back() == '.' || lineText.back() == ':');
    if (items.empty() || (prefix.empty() && !afterSep)) {
        dismissPopup();
        return;
    }
    m_popupPrefix = prefix;
    if (m_popupSel >= int(items.size())) m_popupSel = int(items.size()) - 1;
    if (m_popupSel < 0) m_popupSel = 0;

    // --- key handling (keys are stolen from the widget this frame) ---
    ImGuiIO& io = ImGui::GetIO();
    bool accept = false;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        m_popupSel = (m_popupSel + 1) % int(items.size());
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        m_popupSel = (m_popupSel - 1 + int(items.size())) % int(items.size());
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        dismissPopup();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        accept = true;
    (void)io;

    if (accept) {
        const CompletionItem& it = items[size_t(m_popupSel)];
        insertCompletionRemainder(doc.editor.get(), it.insert, m_popupPrefix);
        dismissPopup();
        return;
    }

    // --- positioning: anchor under the text cursor ---
    // Monospace char advance from the editor's font ('#' is full-width).
    const float charW = ImGui::CalcTextSize("#").x;
    // The widget renders with ItemSpacing pushed to (0,0), so its per-line
    // advance is the bare font height — NOT GetTextLineHeightWithSpacing(),
    // whose spacing term would accumulate into vertical drift down the file.
    const float lineH = ImGui::GetTextLineHeight();
    // Gutter width: " <maxline> " plus the widget's mLeftMargin (10px,
    // hardcoded upstream) — mirrors the private mTextStart computation.
    char buf[16];
    std::snprintf(buf, sizeof(buf), " %d ", doc.editor->GetTotalLines());
    const float gutter = ImGui::CalcTextSize(buf).x + 10.0f;
    const ImVec2 origin = m_lastEditorOrigin;
    ImVec2 pos(origin.x + gutter + float(cur.mColumn) * charW -
                   m_lastEditorScroll.x,
               origin.y + float(cur.mLine + 1) * lineH - m_lastEditorScroll.y);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSizeConstraints(ImVec2(180, 0), ImVec2(420, 220));
    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::SetNextWindowBgAlpha(0.96f);
    if (ImGui::Begin("##completions", nullptr, popupFlags)) {
        for (int i = 0; i < int(items.size()); ++i) {
            const CompletionItem& it = items[size_t(i)];
            const bool sel = i == m_popupSel;
            if (ImGui::Selectable(it.label.c_str(), sel)) {
                m_popupSel = i;
                // mouse-click accept on next frame is awkward; accept inline.
                insertCompletionRemainder(doc.editor.get(), it.insert,
                                          m_popupPrefix);
                dismissPopup();
            }
            if (!it.detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", it.detail.c_str());
            }
            if (sel) ImGui::SetScrollHereY();
        }
    }
    ImGui::End();
}

} // namespace liminal::editor
