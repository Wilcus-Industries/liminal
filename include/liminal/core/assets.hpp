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

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace liminal {

class PakReader;

class Assets {
public:
    // Appends a directory to the search list (no-op if already present).
    static void addSearchPath(const std::string& dir);

    // First existing <searchPath>/<relpath>, else relpath unchanged.
    // An absolute path that exists is returned as-is. Disk-only — pak contents
    // are NOT visible here; callers that may be served from a pak should use
    // readFile() instead.
    static std::string resolve(const std::string& relpath);

    // The current list, env root (if any) first.
    static const std::vector<std::string>& searchPaths();

    // Mount a pak (shared so the same reader can back many lookups). Checked
    // BEFORE the env root / search paths so a shipped game's bundled assets
    // can never be shadowed by a stray file on disk. Pass null to unmount.
    static void mountPak(std::shared_ptr<PakReader> pak);

    // True once a pak is mounted.
    static bool hasPak();

    // Read a file's bytes: pak first (normalized lookup), then resolve() + a
    // binary ifstream off disk. nullopt if neither source has it. This is the
    // single VFS entry point — engine code that wants pak-transparency reads
    // through here rather than opening files directly.
    static std::optional<std::string> readFile(const std::string& relpath);
};

} // namespace liminal
