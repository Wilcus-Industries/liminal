// assets.cpp — ordered search-path asset resolution.
//
// Not thread-safe by design: paths are registered during startup, resolve()
// is called from wherever, and the list never shrinks. If an app ever needs
// to register paths from worker threads, that's the app's lock to take.

#include <liminal/core/assets.hpp>

#include <liminal/core/pak.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>

namespace liminal {

namespace {

// Single mounted pak (startup-set, like the search paths — not thread-safe by
// the same design rationale; mount during init, read from anywhere after).
std::shared_ptr<PakReader>& mountedPak() {
    static std::shared_ptr<PakReader> pak;
    return pak;
}

std::vector<std::string>& paths() {
    // env root seeded once, lazily, so addSearchPath() calls before/after
    // main()'s first resolve all see the same ordering rule: env first.
    static std::vector<std::string> list = [] {
        std::vector<std::string> p;
        if (const char* env = std::getenv("LIMINAL_ASSET_ROOT")) {
            p.emplace_back(env);
        }
        return p;
    }();
    return list;
}

} // namespace

void Assets::addSearchPath(const std::string& dir) {
    auto& p = paths();
    if (std::find(p.begin(), p.end(), dir) == p.end()) {
        p.push_back(dir);
    }
}

std::string Assets::resolve(const std::string& relpath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::path(relpath).is_absolute() && fs::exists(relpath, ec)) {
        return relpath;
    }
    for (const auto& dir : paths()) {
        std::string candidate = dir + "/" + relpath;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return relpath;
}

const std::vector<std::string>& Assets::searchPaths() {
    return paths();
}

void Assets::mountPak(std::shared_ptr<PakReader> pak) {
    mountedPak() = std::move(pak);
}

bool Assets::hasPak() {
    return static_cast<bool>(mountedPak());
}

std::optional<std::string> Assets::readFile(const std::string& relpath) {
    // Pak wins over disk: a shipped game must not be shadowed by stray files.
    if (const auto& pak = mountedPak()) {
        if (auto bytes = pak->read(relpath)) return bytes;
    }
    const std::filesystem::path resolved = resolve(relpath);
    // Guard against opening a directory: an ifstream on a dir can "succeed"
    // on some platforms and then yield junk; only read regular files.
    if (!std::filesystem::is_regular_file(resolved)) return std::nullopt;
    std::ifstream in(resolved, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace liminal
