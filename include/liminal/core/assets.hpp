#pragma once
// Runtime asset path resolution. The engine itself reads no assets (the
// default shader pack is embedded), but apps need a portable way to find
// theirs. Ordered search paths, tried first-registered-first:
//
//   liminal::Assets::addSearchPath("/path/to/assets");
//   std::string p = liminal::Assets::resolve("textures/foo.png");
//
// $LIMINAL_ASSET_ROOT, when set, is implicitly the highest-priority search
// path. resolve() returns the first <searchPath>/<relpath> that exists on
// disk; if nothing matches, it returns relpath unchanged so the caller's
// open fails loudly with the name it asked for.

#include <string>
#include <vector>

namespace liminal {

class Assets {
public:
    // Appends a directory to the search list (no-op if already present).
    static void addSearchPath(const std::string& dir);

    // First existing <searchPath>/<relpath>, else relpath unchanged.
    // An absolute path that exists is returned as-is.
    static std::string resolve(const std::string& relpath);

    // The current list, env root (if any) first.
    static const std::vector<std::string>& searchPaths();
};

} // namespace liminal
