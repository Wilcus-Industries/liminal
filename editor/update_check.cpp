#include "update_check.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace liminal::editor {
namespace {

// GitHub's /releases/latest returns the latest published, non-prerelease,
// non-draft release — exactly "latest stable". A repo with no published
// Release returns 404, which curl -f turns into a non-zero exit (→ "").
constexpr const char* kReleasesApiUrl =
    "https://api.github.com/repos/wilcus-industries/liminal/releases/latest";

} // namespace

bool parseSemver(const std::string& s, int& maj, int& min, int& patch) {
    size_t i = 0;
    while (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i; // strip leading v
    int parts[3] = {0, 0, 0};
    for (int p = 0; p < 3; ++p) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
        long val = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            val = val * 10 + (s[i] - '0');
            ++i;
        }
        parts[p] = static_cast<int>(val);
        if (p < 2) {
            if (i >= s.size() || s[i] != '.') return false; // need "X.Y."
            ++i;
        }
        // After patch, ignore any "-rc1"/"+build" suffix (i may dangle).
    }
    maj = parts[0];
    min = parts[1];
    patch = parts[2];
    return true;
}

bool isNewer(const std::string& latest, const std::string& current) {
    int lMaj, lMin, lPat, cMaj, cMin, cPat;
    if (!parseSemver(latest, lMaj, lMin, lPat)) return false;
    if (!parseSemver(current, cMaj, cMin, cPat)) return false;
    if (lMaj != cMaj) return lMaj > cMaj;
    if (lMin != cMin) return lMin > cMin;
    return lPat > cPat;
}

std::string fetchLatestReleaseTag() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) / "liminal_update_check.json";
    if (ec) return "";

    // URL is a compile-time constant — no user input reaches the shell, so no
    // injection surface. -f: HTTP errors (e.g. 404) become a non-zero exit.
    // GitHub rejects requests without a User-Agent header.
    std::string cmd =
        "curl -fsSL --max-time 5 "
        "-H \"Accept: application/vnd.github+json\" "
        "-H \"User-Agent: liminal-editor\" "
        "\"" + std::string(kReleasesApiUrl) + "\" -o \"" + tmp.string() + "\"";
#ifdef _WIN32
    cmd += " >NUL 2>&1";
#else
    cmd += " >/dev/null 2>&1";
#endif

    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        fs::remove(tmp, ec);
        return "";
    }

    std::string tag;
    try {
        std::ifstream in(tmp);
        nlohmann::json j;
        in >> j;
        tag = j.value("tag_name", "");
    } catch (...) {
        tag.clear();
    }
    fs::remove(tmp, ec);
    return tag;
}

} // namespace liminal::editor
