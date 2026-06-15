#pragma once
// Pty: an RAII pseudo-terminal master paired with a child process running on
// the slave side. Used by the editor's terminal panel to host an interactive
// program (Claude Code's `claude`, or a shell) with full TUI behaviour —
// 256-color, cursor movement, alt-screen — by feeding the child's output into
// a VT engine (libvterm) and writing keystrokes back.
//
// Platform:
//   macOS / Linux — forkpty (<util.h> / <pty.h>). The master fd is set
//   O_NONBLOCK so read() never blocks the caller; the panel owns its own
//   reader thread but the fd discipline keeps even a synchronous drain safe.
//   Windows — stubbed (ConPTY is the eventual implementation). spawn() returns
//   false and the object stays inert; see the CLAUDE.md accepted-limitation
//   note.
//
// Threading model: Pty itself holds no locks and spawns no threads. Its
// methods are plain syscalls over the master fd / child pid and are NOT
// internally synchronized — the owner serializes access. The terminal panel's
// model is: a dedicated reader thread calls read() in a loop and parks bytes
// in its own mutex-guarded queue, while the main (ImGui) thread calls write()
// / resize() / alive(). read() on one thread concurrent with write() on
// another is fine (independent fd directions); two writers are not. This
// mirrors the audio discipline of keeping the realtime/worker side off shared
// mutable state.

#include <string>
#include <vector>

namespace liminal {

class Pty {
public:
    Pty() = default;
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    Pty(Pty&&) noexcept;
    Pty& operator=(Pty&&) noexcept;

    // Fork a child on the slave side and exec `cmd` with `args` (argv[0] is set
    // to `cmd`; `args` are the trailing argv entries). The child's environment
    // inherits ours plus TERM=xterm-256color. If `cwd` is non-empty the child
    // chdir()s into it before exec (a chdir failure is non-fatal — the child
    // proceeds in the inherited directory). The master fd is made non-blocking.
    // Returns false on any failure (and leaves the object inert).
    bool spawn(const std::string& cmd, const std::vector<std::string>& args,
               int cols, int rows, const std::string& cwd = {});

    // Drain whatever is currently available on the master into `buf` (appended).
    // Returns the number of bytes read (>0), 0 if nothing was waiting, or -1 on
    // EOF / unrecoverable error (the child has gone or the fd closed).
    long read(std::vector<unsigned char>& buf);

    // Write all of `bytes` to the master (blocking until the fd accepts it, or
    // returning false on error). Empty input is a no-op success.
    bool write(const unsigned char* bytes, size_t n);
    bool write(const std::string& s) {
        return write(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    }

    // Push a new window size to the slave (TIOCSWINSZ); the child sees SIGWINCH.
    void resize(int cols, int rows);

    // True while the child has not been reaped (waitpid WNOHANG). Once the child
    // exits this latches false.
    bool alive();

    bool valid() const { return m_master >= 0; }

private:
    void shutdown(); // close fd, kill+reap child; idempotent

    int m_master = -1;       // master fd, -1 = none
    long m_child = -1;       // child pid (intptr-wide for the Windows stub), -1 = none
    bool m_reaped = false;   // child observed exited
};

} // namespace liminal
