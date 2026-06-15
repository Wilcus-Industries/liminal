#include <liminal/core/platform.hpp>

#include <filesystem>

#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <shellapi.h>
#include <windows.h>
#include <vector>
#elif defined(__APPLE__)
#include <climits> // PATH_MAX
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#include <vector>
#endif

namespace liminal {

namespace fs = std::filesystem;

std::string selfExePath() {
#if defined(_WIN32)
    // GetModuleFileNameW into a growing buffer (path may exceed MAX_PATH).
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                       static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            // fs::path consumes the wide string and yields UTF-8 via generic.
            return fs::path(std::wstring(buf.data(), n)).generic_string();
        }
        buf.resize(buf.size() * 2); // truncated — grow and retry
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // first call reports needed size
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    // The returned path may contain symlinks / "../"; realpath canonicalizes.
    char resolved[PATH_MAX];
    if (::realpath(buf.data(), resolved) != nullptr) {
        return fs::path(resolved).generic_string();
    }
    return fs::path(buf.data()).generic_string();
#else
    // Linux and other /proc-bearing systems.
    std::vector<char> buf(4096);
    for (;;) {
        ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return {};
        if (static_cast<size_t>(n) < buf.size()) {
            return fs::path(std::string(buf.data(), static_cast<size_t>(n)))
                .generic_string();
        }
        buf.resize(buf.size() * 2); // possibly truncated — grow and retry
    }
#endif
}

std::string selfExeDir() {
    const std::string exe = selfExePath();
    if (exe.empty()) return {};
    return fs::path(exe).parent_path().generic_string();
}

bool openUrl(const std::string& url) {
    if (url.empty()) return false;
#if defined(_WIN32)
    // ShellExecute with the "open" verb hands the URL to the default handler.
    HINSTANCE r = ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
#else
#if defined(__APPLE__)
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    // Single-quote the URL and escape any embedded single quotes so the shell
    // treats it as one literal argument (defends against odd characters in a
    // path-as-URL; our repo link is a constant either way).
    std::string quoted = "'";
    for (char c : url) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += "'";
    const std::string cmd = std::string(opener) + " " + quoted + " >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#endif
}

std::filesystem::path userConfigDir() {
    fs::path base;
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"))
        base = fs::path(appdata) / "liminal";
#else
    if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".liminal";
#endif
    if (base.empty()) return {};
    std::error_code ec;
    fs::create_directories(base, ec); // non-fatal; existence check below
    if (ec && !fs::exists(base)) return {};
    return base;
}

} // namespace liminal
