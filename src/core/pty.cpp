// Pty implementation. POSIX (macOS / Linux) via forkpty; Windows stubbed.
//
// The fd discipline is the whole game here: the master is O_NONBLOCK so a
// read() with nothing waiting returns EAGAIN (reported as 0 bytes, not an
// error), and only a genuine EOF (child closed its end) or a hard errno is
// surfaced as -1. write() loops on partial writes / EAGAIN because a TUI can
// hand us a paste larger than the kernel's tty buffer.

#include <liminal/core/pty.hpp>

#if defined(_WIN32)
// Windows: ConPTY is the eventual home; until then Pty is inert. The terminal
// panel surfaces "unsupported on this platform" when spawn() returns false.
#else
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <util.h> // forkpty
#else
#include <pty.h>  // forkpty (glibc)
#endif
#endif

namespace liminal {

#if defined(_WIN32)

// --- Windows stub ------------------------------------------------------------
// Every method no-ops / reports unsupported; valid() stays false so callers
// treat the panel as a dead terminal.

Pty::~Pty() {}
Pty::Pty(Pty&&) noexcept {}
Pty& Pty::operator=(Pty&&) noexcept { return *this; }

bool Pty::spawn(const std::string&, const std::vector<std::string>&, int, int) {
    return false; // ConPTY not yet implemented
}
long Pty::read(std::vector<unsigned char>&) { return -1; }
bool Pty::write(const unsigned char*, size_t) { return false; }
void Pty::resize(int, int) {}
bool Pty::alive() { return false; }
void Pty::shutdown() {}

#else

// --- POSIX -------------------------------------------------------------------

Pty::~Pty() { shutdown(); }

Pty::Pty(Pty&& other) noexcept
    : m_master(other.m_master), m_child(other.m_child),
      m_reaped(other.m_reaped) {
    other.m_master = -1;
    other.m_child = -1;
    other.m_reaped = false;
}

Pty& Pty::operator=(Pty&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_master = other.m_master;
        m_child = other.m_child;
        m_reaped = other.m_reaped;
        other.m_master = -1;
        other.m_child = -1;
        other.m_reaped = false;
    }
    return *this;
}

bool Pty::spawn(const std::string& cmd, const std::vector<std::string>& args,
                int cols, int rows, const std::string& cwd) {
    if (valid()) return false; // already running

    // Seed the slave with the requested window size so the child's first
    // ioctl(TIOCGWINSZ) (and any TUI that reads it at startup) sees the truth.
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);

    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return false; // fork/openpty failed

    if (pid == 0) {
        // --- child: on the slave side of the pty, stdio already redirected ---
        ::setenv("TERM", "xterm-256color", 1);
        // Start in the requested directory (the opened project) if given; a
        // failed chdir is non-fatal — fall through in the inherited cwd.
        if (!cwd.empty()) (void)::chdir(cwd.c_str());
        // execvp wants a NULL-terminated, mutable argv. argv[0] = cmd.
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(cmd.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp(cmd.c_str(), argv.data());
        // Only reached if exec failed; the parent will observe EOF on the master.
        ::_exit(127);
    }

    // --- parent: own the master, make reads non-blocking ---
    const int flags = ::fcntl(master, F_GETFL, 0);
    if (flags >= 0) ::fcntl(master, F_SETFL, flags | O_NONBLOCK);

    m_master = master;
    m_child = pid;
    m_reaped = false;
    return true;
}

long Pty::read(std::vector<unsigned char>& buf) {
    if (m_master < 0) return -1;

    unsigned char chunk[8192];
    long total = 0;
    for (;;) {
        const ssize_t n = ::read(m_master, chunk, sizeof(chunk));
        if (n > 0) {
            buf.insert(buf.end(), chunk, chunk + n);
            total += n;
            if (static_cast<size_t>(n) < sizeof(chunk)) break; // drained
            continue; // a full chunk: there may be more
        }
        if (n == 0) return -1; // EOF: slave fully closed
        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return total; // nothing (more) waiting right now
        if (errno == EINTR) continue;
        return -1; // hard error
    }
    return total;
}

bool Pty::write(const unsigned char* bytes, size_t n) {
    if (m_master < 0) return false;
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(m_master, bytes + off, n - off);
        if (w > 0) {
            off += static_cast<size_t>(w);
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The tty input buffer is momentarily full; yield and retry rather
            // than spin. Callers feed at human/paste rates so this is rare.
            ::usleep(1000);
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void Pty::resize(int cols, int rows) {
    if (m_master < 0) return;
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
    ::ioctl(m_master, TIOCSWINSZ, &ws);
}

bool Pty::alive() {
    if (m_child < 0) return false;
    if (m_reaped) return false;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(m_child), &status, WNOHANG);
    if (r == 0) return true;       // still running
    if (r == static_cast<pid_t>(m_child)) {
        m_reaped = true;           // exited and now reaped
        return false;
    }
    // r < 0: ECHILD (already reaped elsewhere) or error — treat as dead.
    m_reaped = true;
    return false;
}

void Pty::shutdown() {
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    if (m_child >= 0 && !m_reaped) {
        // Ask politely, then reap. The child is interactive (claude / a shell);
        // SIGHUP on a closed pty usually suffices, SIGKILL is the seatbelt.
        ::kill(static_cast<pid_t>(m_child), SIGHUP);
        int status = 0;
        if (::waitpid(static_cast<pid_t>(m_child), &status, WNOHANG) == 0) {
            ::kill(static_cast<pid_t>(m_child), SIGKILL);
            ::waitpid(static_cast<pid_t>(m_child), &status, 0);
        }
    }
    m_child = -1;
    m_reaped = true;
}

#endif // _WIN32

} // namespace liminal
