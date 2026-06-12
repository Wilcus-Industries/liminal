// assets.cpp — ordered search-path asset resolution.
//
// Not thread-safe by design: paths are registered during startup, resolve()
// is called from wherever, and the list never shrinks. If an app ever needs
// to register paths from worker threads, that's the app's lock to take.

#include <liminal/core/assets.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace liminal {

namespace {

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

} // namespace liminal
