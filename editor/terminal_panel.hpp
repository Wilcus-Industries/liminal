#pragma once
// TerminalPanel: the "Terminal" dock window — a real VT100/xterm-256color
// terminal emulator hosting an interactive child process (the user's login
// shell, $SHELL or /bin/zsh -l). It owns a liminal::Pty for the
// child and a libvterm VTerm for VT parsing/screen state; the panel renders the
// screen grid cell-by-cell into the ImGui draw list and translates focused
// keyboard/clipboard input back into the pty.
//
// Threading model (mirrors the audio discipline of keeping the worker side off
// shared mutable state): a dedicated reader thread loops on Pty::read() and
// parks raw bytes in a mutex-guarded queue. The main (ImGui) thread is the only
// one that touches the VTerm — each draw() it swaps the queue out under the
// lock, then feeds the bytes into vterm_input_write and reads cells. So the VT
// engine is single-threaded by construction; only the byte queue + a couple of
// atomics cross the thread boundary. Pty::write()/resize() happen on the main
// thread while the reader thread only ever Pty::read()s (independent fd
// directions — safe).

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <liminal/core/pty.hpp>

// libvterm is a C library; its public types (VTerm, VTermScreen, VTermScreenCell
// …) are anonymous-struct typedefs that cannot be forward-declared, so we pull
// the header in here under extern "C".
extern "C" {
#include <vterm.h>
}

namespace liminal::editor {

class TerminalPanel {
public:
    TerminalPanel();
    ~TerminalPanel();

    TerminalPanel(const TerminalPanel&) = delete;
    TerminalPanel& operator=(const TerminalPanel&) = delete;

    // Draw the dock window. Call once per frame from EditorApp::drawUi. Spawns
    // the child lazily on the first draw that has a known cell grid AND a
    // working directory set (so `claude` starts in the opened project).
    void draw();

    // Set the directory the child process is spawned in (the opened project's
    // dir). Must be called before the session spawns; once the child is running
    // this is a no-op. EditorApp calls it on project open.
    void setWorkingDir(const std::string& dir) {
        if (!m_started) m_workingDir = dir;
    }

    // Kill the child + join the reader thread (no-op if never spawned). Also
    // run by the dtor; exposed so EditorApp::closeProject can tear the session
    // down before recreating a fresh panel.
    void stopSession();

private:
    // One scrolled-off line, captured from libvterm's sb_pushline callback.
    struct ScrollLine {
        std::vector<uint32_t> chars; // one codepoint per cell (0 = blank)
        std::vector<uint32_t> fg;    // packed 0xRRGGBB
        std::vector<uint32_t> bg;
    };

    void startSession(int cols, int rows);  // create Pty + VTerm + reader thread
    void pumpInput();                        // drain queue -> vterm_input_write
    void renderGrid(int cols, int rows, bool focused); // cells -> ImGui draw list
    void handleInput();                      // focused keyboard/clipboard -> pty
    void ensureSize(int cols, int rows);     // vterm_set_size + Pty::resize

    // Extract the active mouse selection as text (visible screen only;
    // scrollback excluded). Reading-order span, rows joined with "\n".
    std::string selectedText() const;

    // libvterm screen scrollback callbacks (static thunks -> members).
    static int sbPushThunk(int cols, const VTermScreenCell* cells, void* user);
    static int sbPopThunk(int cols, VTermScreenCell* cells, void* user);
    int sbPush(int cols, const VTermScreenCell* cells);
    int sbPop(int cols, VTermScreenCell* cells);

    // --- child + VT engine (all VTerm access is main-thread only) ---
    Pty m_pty;
    VTerm* m_vt = nullptr;
    VTermScreen* m_screen = nullptr;
    VTermState* m_state = nullptr;
    int m_cols = 0;
    int m_rows = 0;
    bool m_started = false;            // session ever started
    bool m_exited = false;             // child observed gone
    std::string m_status;             // banner line (cmd launched / exit notice)
    std::string m_workingDir;          // child cwd (opened project dir); empty = wait

    // --- reader thread + byte queue (the only cross-thread shared state) ---
    std::thread m_reader;
    std::atomic<bool> m_readerRun{false};
    std::atomic<bool> m_readerEof{false};
    std::mutex m_queueMutex;
    std::vector<unsigned char> m_queue; // raw pty output awaiting the VT engine

    // --- scrollback (main-thread only) ---
    std::deque<ScrollLine> m_scrollback; // newest at back; capped
    int m_scrollOffset = 0;              // lines scrolled up from the live view

    bool m_focusedLastFrame = false;

    // --- mouse text selection (visible-screen cell coords, row 0 = top) ---
    bool m_selecting = false; // drag in progress
    bool m_hasSel = false;    // a committed/active selection exists
    int m_selAnchorRow = 0, m_selAnchorCol = 0; // drag origin
    int m_selEndRow = 0, m_selEndCol = 0;       // current/last drag point
};

} // namespace liminal::editor
