// TerminalPanel implementation. See terminal_panel.hpp for the threading model.
//
// libvterm notes:
//   - The VTerm is single-threaded: only the main thread (draw()) ever touches
//     it. The reader thread does nothing but Pty::read() into a locked byte
//     queue; draw() swaps the queue out and feeds it to vterm_input_write.
//   - We drive keyboard input through libvterm's own vterm_keyboard_* calls and
//     then drain vterm_output_read for the bytes to send to the pty — that lets
//     libvterm own the (mode-dependent) CSI/SS3 encoding instead of us
//     hand-rolling it. Printable text goes through vterm_keyboard_unichar.
//   - Colors: each cell carries fg/bg as VTermColor. vterm_screen_convert_color
//     _to_rgb resolves indexed / default colors against the palette, after which
//     VTERM_COLOR_IS_RGB is true and we pack 0xRRGGBB.

#include "terminal_panel.hpp"

#include <imgui.h>
#include <imgui_internal.h> // SetKeyOwner to steal keys while focused

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace liminal::editor {

namespace {

// Scrollback cap (lines). A ring: oldest dropped past this.
constexpr size_t kScrollbackMax = 5000;

// xterm's default RGB for the 16 ANSI colors, used only as a last-resort
// palette before libvterm resolves its own (it normally does so itself via
// convert_color_to_rgb; this is dead-simple defensive packing).
inline ImU32 packRGB(uint8_t r, uint8_t g, uint8_t b) {
    return IM_COL32(r, g, b, 255);
}

// Resolve a VTermColor to packed 0xRRGGBB via the screen's palette, returning
// it as an ImU32. defaultIsBg picks the terminal default fg/bg when the cell
// carries a "default" color.
ImU32 colorToImU32(VTermScreen* screen, VTermColor col, bool isBg) {
    if (VTERM_COLOR_IS_DEFAULT_FG(&col) || VTERM_COLOR_IS_DEFAULT_BG(&col)) {
        // Defaults: dark background, light foreground — a classic terminal look.
        return isBg ? packRGB(18, 18, 22) : packRGB(220, 220, 220);
    }
    vterm_screen_convert_color_to_rgb(screen, &col);
    if (VTERM_COLOR_IS_RGB(&col))
        return packRGB(col.rgb.red, col.rgb.green, col.rgb.blue);
    return isBg ? packRGB(18, 18, 22) : packRGB(220, 220, 220);
}

} // namespace

// --- lifecycle ---------------------------------------------------------------

TerminalPanel::TerminalPanel() = default;

TerminalPanel::~TerminalPanel() { stopSession(); }

void TerminalPanel::startSession(int cols, int rows) {
    if (m_started) return;
    m_cols = std::max(cols, 1);
    m_rows = std::max(rows, 1);

    // Launch the user's login shell (login so PATH/aliases are set up).
    const char* sh = std::getenv("SHELL");
    std::string cmd = (sh && *sh) ? sh : "/bin/zsh";
    std::vector<std::string> args{"-l"};
    m_status = "launched shell: " + cmd;

    if (!m_pty.spawn(cmd, args, m_cols, m_rows, m_workingDir)) {
        m_status = "failed to spawn a terminal (unsupported on this platform?)";
        m_started = true; // don't retry every frame
        m_exited = true;
        return;
    }

    // VTerm: libvterm takes (rows, cols). UTF-8 input, scrollback callbacks.
    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    m_screen = vterm_obtain_screen(m_vt);
    m_state = vterm_obtain_state(m_vt);

    static const VTermScreenCallbacks kCbs = {
        nullptr,            // damage
        nullptr,            // moverect
        nullptr,            // movecursor
        nullptr,            // settermprop
        nullptr,            // bell
        nullptr,            // resize
        &TerminalPanel::sbPushThunk, // sb_pushline
        &TerminalPanel::sbPopThunk,  // sb_popline
        nullptr,            // sb_clear
        nullptr,            // sb_pushline4
    };
    vterm_screen_set_callbacks(m_screen, &kCbs, this);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_reset(m_screen, 1);

    // Reader thread: nothing but Pty::read() into the locked queue.
    m_readerRun = true;
    m_reader = std::thread([this] {
        std::vector<unsigned char> local;
        while (m_readerRun.load(std::memory_order_relaxed)) {
            local.clear();
            const long n = m_pty.read(local);
            if (n < 0) {
                m_readerEof = true;
                break;
            }
            if (n > 0) {
                std::lock_guard<std::mutex> lk(m_queueMutex);
                m_queue.insert(m_queue.end(), local.begin(), local.end());
            } else {
                // Nothing waiting; sleep briefly so we don't spin a core.
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
    });

    m_started = true;
}

void TerminalPanel::stopSession() {
    m_readerRun = false;
    if (m_reader.joinable()) m_reader.join();
    if (m_vt) {
        vterm_free(m_vt);
        m_vt = nullptr;
        m_screen = nullptr;
        m_state = nullptr;
    }
    // m_pty dtor closes the fd and kills+reaps the child.
}

// --- scrollback callbacks ----------------------------------------------------

int TerminalPanel::sbPushThunk(int cols, const VTermScreenCell* cells, void* user) {
    return static_cast<TerminalPanel*>(user)->sbPush(cols, cells);
}
int TerminalPanel::sbPopThunk(int cols, VTermScreenCell* cells, void* user) {
    return static_cast<TerminalPanel*>(user)->sbPop(cols, cells);
}

int TerminalPanel::sbPush(int cols, const VTermScreenCell* cells) {
    ScrollLine line;
    line.chars.resize(size_t(cols));
    line.fg.resize(size_t(cols));
    line.bg.resize(size_t(cols));
    for (int c = 0; c < cols; ++c) {
        line.chars[size_t(c)] = cells[c].chars[0]; // first codepoint per cell
        // Store resolved RGB so popped lines render without re-resolving.
        VTermColor fg = cells[c].fg, bg = cells[c].bg;
        line.fg[size_t(c)] = colorToImU32(m_screen, fg, false);
        line.bg[size_t(c)] = colorToImU32(m_screen, bg, true);
    }
    m_scrollback.push_back(std::move(line));
    if (m_scrollback.size() > kScrollbackMax) m_scrollback.pop_front();
    return 1;
}

int TerminalPanel::sbPop(int cols, VTermScreenCell* cells) {
    if (m_scrollback.empty()) return 0;
    const ScrollLine& line = m_scrollback.back();
    for (int c = 0; c < cols; ++c) {
        std::memset(&cells[c], 0, sizeof(VTermScreenCell));
        cells[c].width = 1;
        if (size_t(c) < line.chars.size()) cells[c].chars[0] = line.chars[size_t(c)];
    }
    m_scrollback.pop_back();
    return 1;
}

// --- input pump --------------------------------------------------------------

void TerminalPanel::pumpInput() {
    if (!m_vt) return;
    std::vector<unsigned char> bytes;
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        if (m_queue.empty()) return;
        bytes.swap(m_queue);
    }
    vterm_input_write(m_vt, reinterpret_cast<const char*>(bytes.data()),
                      bytes.size());
}

void TerminalPanel::ensureSize(int cols, int rows) {
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);
    if (cols == m_cols && rows == m_rows) return;
    m_cols = cols;
    m_rows = rows;
    if (m_vt) vterm_set_size(m_vt, rows, cols);
    m_pty.resize(cols, rows);
}

// Extract the active selection as text. Covers the VISIBLE screen only
// (scrollback is excluded). vterm_screen_get_text takes a RECTANGULAR region,
// so a reading-order span is pulled per-row: each row's [c0,c1) (c0 = startCol
// on the first row else 0; c1 = endCol+1 on the last row else cols), trailing
// spaces trimmed, rows joined with "\n".
std::string TerminalPanel::selectedText() const {
    if (!m_hasSel || !m_vt) return {};
    VTermScreen* screen = vterm_obtain_screen(m_vt);
    if (!screen) return {};

    int sr = m_selAnchorRow, sc = m_selAnchorCol, er = m_selEndRow, ec = m_selEndCol;
    if (sr > er || (sr == er && sc > ec)) { std::swap(sr, er); std::swap(sc, ec); }

    std::string out;
    std::vector<char> buf;
    for (int r = sr; r <= er; ++r) {
        const int c0 = (r == sr) ? sc : 0;
        const int c1 = (r == er) ? ec + 1 : m_cols;
        if (c1 <= c0) { if (r != er) out.push_back('\n'); continue; }
        const VTermRect rect{r, r + 1, c0, c1};
        const size_t need = vterm_screen_get_text(screen, nullptr, 0, rect);
        buf.assign(need + 1, '\0');
        const size_t got = vterm_screen_get_text(screen, buf.data(), need, rect);
        std::string row(buf.data(), got);
        // Trim trailing spaces from each row (blank cells beyond the text).
        const size_t last = row.find_last_not_of(' ');
        row = (last == std::string::npos) ? std::string() : row.substr(0, last + 1);
        out += row;
        if (r != er) out.push_back('\n');
    }
    return out;
}

// --- keyboard / clipboard ----------------------------------------------------

void TerminalPanel::handleInput() {
    if (!m_vt) return;
    ImGuiIO& io = ImGui::GetIO();

    const VTermModifier mod = VTermModifier(
        (io.KeyShift ? VTERM_MOD_SHIFT : 0) | (io.KeyAlt ? VTERM_MOD_ALT : 0) |
        (io.KeyCtrl ? VTERM_MOD_CTRL : 0));

    // Copy the mouse selection. macOS Cmd+C copies (it must NOT interrupt —
    // the interrupt path below is gated on physical Ctrl && !Super, so Cmd+C
    // never reaches it). Ctrl+C stays the sole interrupt on every platform.
    if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        const std::string s = selectedText();
        if (!s.empty()) ImGui::SetClipboardText(s.c_str());
        return; // don't fall through to the keyboard paths this frame
    }

    // Paste (Cmd/Ctrl+V): bracketed-paste-aware via libvterm.
    if ((io.KeySuper || io.KeyCtrl) && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        if (const char* clip = ImGui::GetClipboardText()) {
            vterm_keyboard_start_paste(m_vt);
            // Decode UTF-8 to code points before feeding vterm_keyboard_unichar:
            // with VTERM_MOD_NONE libvterm re-encodes its argument AS a code
            // point, so passing raw bytes would turn each multibyte lead/cont
            // byte into its own (wrong) character. ASCII is unaffected.
            for (const char* p = clip; *p;) {
                unsigned char c = static_cast<unsigned char>(*p);
                uint32_t cp;
                int len;
                if (c < 0x80) { cp = c; len = 1; }
                else if ((c >> 5) == 0x6) { cp = c & 0x1Fu; len = 2; }
                else if ((c >> 4) == 0xE) { cp = c & 0x0Fu; len = 3; }
                else if ((c >> 3) == 0x1E) { cp = c & 0x07u; len = 4; }
                else { ++p; continue; } // stray continuation / invalid lead
                bool ok = true;
                for (int i = 1; i < len; ++i) {
                    if ((static_cast<unsigned char>(p[i]) & 0xC0u) != 0x80u) {
                        ok = false; // truncated (incl. NUL) or malformed
                        break;
                    }
                    cp = (cp << 6) | (static_cast<unsigned char>(p[i]) & 0x3Fu);
                }
                if (!ok) { ++p; continue; }
                vterm_keyboard_unichar(m_vt, cp, VTERM_MOD_NONE);
                p += len;
            }
            vterm_keyboard_end_paste(m_vt);
        }
    } else {
        // Printable text: ImGui already decoded it to Unicode codepoints. Skip
        // this path when a control/super modifier is held (those map to keys).
        if (!io.KeyCtrl && !io.KeySuper) {
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
                const ImWchar ch = io.InputQueueCharacters[i];
                if (ch != 0) vterm_keyboard_unichar(m_vt, uint32_t(ch), VTERM_MOD_NONE);
            }
        }
    }

    // Special keys -> libvterm key codes (it owns the CSI/SS3 encoding).
    struct KeyMap { ImGuiKey im; VTermKey vt; };
    static const KeyMap kMap[] = {
        {ImGuiKey_Enter, VTERM_KEY_ENTER},     {ImGuiKey_KeypadEnter, VTERM_KEY_ENTER},
        {ImGuiKey_Tab, VTERM_KEY_TAB},         {ImGuiKey_Backspace, VTERM_KEY_BACKSPACE},
        {ImGuiKey_Escape, VTERM_KEY_ESCAPE},   {ImGuiKey_UpArrow, VTERM_KEY_UP},
        {ImGuiKey_DownArrow, VTERM_KEY_DOWN},  {ImGuiKey_LeftArrow, VTERM_KEY_LEFT},
        {ImGuiKey_RightArrow, VTERM_KEY_RIGHT},{ImGuiKey_Insert, VTERM_KEY_INS},
        {ImGuiKey_Delete, VTERM_KEY_DEL},      {ImGuiKey_Home, VTERM_KEY_HOME},
        {ImGuiKey_End, VTERM_KEY_END},         {ImGuiKey_PageUp, VTERM_KEY_PAGEUP},
        {ImGuiKey_PageDown, VTERM_KEY_PAGEDOWN},
    };
    for (const auto& k : kMap) {
        if (ImGui::IsKeyPressed(k.im, true)) vterm_keyboard_key(m_vt, k.vt, mod);
    }

    // Ctrl+letter -> control codes. libvterm encodes these from unichar+CTRL.
    if (io.KeyCtrl && !io.KeySuper) {
        for (ImGuiKey key = ImGuiKey_A; key <= ImGuiKey_Z;
             key = ImGuiKey(int(key) + 1)) {
            if (ImGui::IsKeyPressed(key, false)) {
                const uint32_t letter = uint32_t('a' + (int(key) - int(ImGuiKey_A)));
                vterm_keyboard_unichar(m_vt, letter, VTERM_MOD_CTRL);
            }
        }
    }

    // Drain whatever libvterm produced for the keyboard events and ship it.
    char out[1024];
    for (;;) {
        const size_t n = vterm_output_read(m_vt, out, sizeof(out));
        if (n == 0) break;
        m_pty.write(reinterpret_cast<const unsigned char*>(out), n);
        if (n < sizeof(out)) break;
    }
}

// --- grid render -------------------------------------------------------------

void TerminalPanel::renderGrid(int cols, int rows, bool focused) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    // Monospace cell metrics: advance width of "M", line height = font size.
    const float cellW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "M").x;
    const float cellH = fontSize;

    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Reserve grid space so ImGui scrolling / hit-testing knows our extent.
    ImGui::Dummy(ImVec2(cellW * cols, cellH * rows));

    // How many scrollback lines are shown above the live grid this frame.
    const int sbCount = int(m_scrollback.size());
    const int maxScroll = sbCount;
    m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);

    // Selection: LINEAR (reading-order) span like a real terminal. Normalize
    // anchor/end to (sr,sc) <= (er,ec); a visible cell (row,col) is selected if
    // it falls between those two points in reading order (full lines in between).
    int sr = m_selAnchorRow, sc = m_selAnchorCol, er = m_selEndRow, ec = m_selEndCol;
    if (sr > er || (sr == er && sc > ec)) { std::swap(sr, er); std::swap(sc, ec); }
    auto cellSelected = [&](int row, int col) {
        if (!m_hasSel) return false;
        if (row < sr || row > er) return false;
        if (row == sr && col < sc) return false;
        if (row == er && col > ec) return false;
        return true;
    };
    const ImU32 selBg = IM_COL32(80, 130, 200, 110);

    char utf8[8];
    auto drawCellGlyph = [&](float x, float y, uint32_t cp) {
        if (cp == 0 || cp == ' ') return;
        // Encode the codepoint to UTF-8 for ImGui's text draw.
        int n = 0;
        if (cp < 0x80) {
            utf8[n++] = char(cp);
        } else if (cp < 0x800) {
            utf8[n++] = char(0xC0 | (cp >> 6));
            utf8[n++] = char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            utf8[n++] = char(0xE0 | (cp >> 12));
            utf8[n++] = char(0x80 | ((cp >> 6) & 0x3F));
            utf8[n++] = char(0x80 | (cp & 0x3F));
        } else {
            utf8[n++] = char(0xF0 | (cp >> 18));
            utf8[n++] = char(0x80 | ((cp >> 12) & 0x3F));
            utf8[n++] = char(0x80 | ((cp >> 6) & 0x3F));
            utf8[n++] = char(0x80 | (cp & 0x3F));
        }
        utf8[n] = 0;
    };

    // Visual layout: the top `m_scrollOffset` rows show the last scrollback
    // lines (oldest of the shown set first); the remaining `rows - offset` rows
    // show the top of the live screen. At offset 0 it's a plain live screen.
    const int sbRows = std::min(m_scrollOffset, rows);
    int screenRow = 0; // visual row counter from the top of the panel

    // 1) Scrolled-back lines.
    const int firstSb = sbCount - m_scrollOffset; // index of the topmost shown
    for (int k = 0; k < sbRows; ++k, ++screenRow) {
        const int i = firstSb + k;
        if (i < 0 || i >= sbCount) continue;
        const ScrollLine& line = m_scrollback[size_t(i)];
        const float y = origin.y + screenRow * cellH;
        for (int c = 0; c < cols && size_t(c) < line.chars.size(); ++c) {
            const float x = origin.x + c * cellW;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cellW, y + cellH),
                              line.bg[size_t(c)]);
            if (cellSelected(screenRow, c)) // translucent over bg, glyph stays
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cellW, y + cellH), selBg);
            const uint32_t cp = line.chars[size_t(c)];
            if (cp && cp != ' ') {
                drawCellGlyph(x, y, cp);
                dl->AddText(font, fontSize, ImVec2(x, y), line.fg[size_t(c)], utf8);
            }
        }
    }

    // 2) Live screen rows, from the top, filling the rest of the panel.
    for (int liveRow = 0; screenRow < rows && liveRow < rows; ++liveRow, ++screenRow) {
        const float y = origin.y + screenRow * cellH;
        for (int c = 0; c < cols; ++c) {
            VTermPos pos{liveRow, c};
            VTermScreenCell cell;
            if (!vterm_screen_get_cell(m_screen, pos, &cell)) continue;
            if (cell.width == 0) continue; // right half of a wide glyph
            const float x = origin.x + c * cellW;
            const float w = cellW * (cell.width > 0 ? cell.width : 1);

            VTermColor fg = cell.fg, bg = cell.bg;
            if (cell.attrs.reverse) std::swap(fg, bg);
            const ImU32 bgCol = colorToImU32(m_screen, bg, true);
            const ImU32 fgCol = colorToImU32(m_screen, fg, false);

            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + cellH), bgCol);
            if (cellSelected(screenRow, c)) // translucent over bg, glyph stays
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + cellH), selBg);
            const uint32_t cp = cell.chars[0];
            if (cp && cp != ' ') {
                drawCellGlyph(x, y, cp);
                dl->AddText(font, fontSize, ImVec2(x, y), fgCol, utf8);
            }
        }
    }

    // 3) Cursor. Its visual row is offset by the scrollback rows shown above the
    //    live screen; hide it if that pushes it off the panel. Solid block when
    //    focused (with the glyph re-drawn in the bg color so it stays legible),
    //    hollow outline when not — the usual focused/unfocused terminal look.
    if (m_state) {
        VTermPos cur;
        vterm_state_get_cursorpos(m_state, &cur);
        const int visRow = cur.row + sbRows;
        if (cur.row >= 0 && visRow < rows && cur.col >= 0 && cur.col < cols) {
            const float x = origin.x + cur.col * cellW;
            const float y = origin.y + visRow * cellH;
            const ImU32 cursorCol = packRGB(220, 220, 220);
            if (focused) {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cellW, y + cellH),
                                  cursorCol);
                VTermScreenCell cell;
                if (vterm_screen_get_cell(m_screen, cur, &cell)) {
                    const uint32_t cp = cell.chars[0];
                    if (cp && cp != ' ') {
                        drawCellGlyph(x, y, cp);
                        dl->AddText(font, fontSize, ImVec2(x, y),
                                    packRGB(18, 18, 22), utf8);
                    }
                }
            } else {
                dl->AddRect(ImVec2(x, y), ImVec2(x + cellW, y + cellH), cursorCol,
                            0.0f, 0, 1.0f);
            }
        }
    }
}

// --- draw --------------------------------------------------------------------

void TerminalPanel::draw() {
    ImGui::Begin("Terminal");
    // ImGui only focuses windows on left-click; focus on right-click too.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsWindowFocused()) ImGui::SetWindowFocus();

    const bool focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // Cell metrics from the current (monospace) font, to size the grid.
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float cellW =
        std::max(1.0f, font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "M").x);
    const float cellH = std::max(1.0f, fontSize);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int cols = std::max(1, int(avail.x / cellW));
    const int rows = std::max(1, int(avail.y / cellH));

    // Lazy spawn on the first frame with a real grid — but only once a working
    // directory (the opened project) is known, so `claude` starts in the
    // project. Until then show a waiting banner.
    if (!m_started) {
        if (m_workingDir.empty()) {
            ImGui::TextUnformatted("Open a project to start the terminal.");
            ImGui::End();
            return;
        }
        startSession(cols, rows);
    } else {
        ensureSize(cols, rows);
    }

    // Pull child output into the VT engine.
    pumpInput();

    // Child exit detection (latched once).
    if (m_started && !m_exited &&
        (m_readerEof.load() || !m_pty.alive())) {
        // Drain any final bytes already queued before declaring exit.
        pumpInput();
        m_exited = true;
        m_status = "session ended — close and reopen the editor to restart";
    }

    // Mouse-wheel scrollback while hovered.
    if (ImGui::IsWindowHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            m_scrollOffset = std::clamp(
                m_scrollOffset + int(wheel * 3.0f), 0, int(m_scrollback.size()));
    }

    // Mouse text selection. Map the cursor to a visible cell using the SAME
    // grid origin (cursor screen pos here = renderGrid's `origin`) + cell size.
    // Runs only while hovered/dragging so it can't steal clicks from other
    // panels; does not touch the wheel.
    if (m_screen) {
        const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        auto mouseCell = [&](int& row, int& col) {
            col = std::clamp(int((mouse.x - gridOrigin.x) / cellW), 0, cols - 1);
            row = std::clamp(int((mouse.y - gridOrigin.y) / cellH), 0, rows - 1);
        };
        if (ImGui::IsWindowHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            mouseCell(m_selAnchorRow, m_selAnchorCol);
            m_selEndRow = m_selAnchorRow;
            m_selEndCol = m_selAnchorCol;
            m_selecting = true;
            m_hasSel = true;
        }
        if (m_selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            mouseCell(m_selEndRow, m_selEndCol);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_selecting = false;
            // A plain click (no drag) clears the selection.
            if (m_selAnchorRow == m_selEndRow && m_selAnchorCol == m_selEndCol)
                m_hasSel = false;
        }
    }

    if (m_screen) renderGrid(cols, rows, focused);
    else ImGui::TextUnformatted(m_status.c_str());

    // Steal keyboard input while focused so the editor's global shortcuts
    // (gizmo W/E/R, Cmd+S, etc.) don't fire underneath the terminal. Mirrors
    // the script editor's key-stealing during its completion popup.
    if (focused) {
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        handleInput();
    }

    if (m_exited && !m_status.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", m_status.c_str());
    }

    ImGui::End();
}

} // namespace liminal::editor
