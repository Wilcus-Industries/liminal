#pragma once
// Relocatable resource resolution for the editor.
//
// The editor needs a handful of on-disk resources at runtime: the JetBrains
// Mono + Noto Color Emoji TTFs, the canonical liminal-lua Claude skill, and the
// bundled sample project. At configure time CMake bakes ABSOLUTE paths into
// these (LIMINAL_EDITOR_FONT_TTF etc.) pointing into the build tree / FetchContent
// _deps — perfect for a developer running straight out of `build/`, useless once
// the executable is copied to another machine.
//
// resolveResource() prefers a copy shipped ALONGSIDE the executable (a packaged
// `.app` puts them in Contents/Resources; a portable dir uses ./resources), and
// falls back to the baked absolute path so in-tree dev builds keep working with
// zero packaging. Callers pass the bundle-relative name + the baked macro.

#include <filesystem>
#include <string>
#include <system_error>

#include <liminal/core/platform.hpp>

namespace liminal::editor {

// Resolve a resource by its bundle-relative name, falling back to a baked
// absolute path. Returns the first existing candidate (canonicalized), else the
// baked path (which may itself be missing — callers already guard with
// fs::exists). `bakedAbs` may be null/empty when no baked path was configured.
inline std::string resolveResource(const std::string& bundleRelName,
                                   const char* bakedAbs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path exeDir(liminal::selfExeDir());
    if (!exeDir.empty()) {
        const fs::path candidates[] = {
            exeDir / ".." / "Resources" / bundleRelName, // macOS .app layout
            exeDir / "resources" / bundleRelName,        // portable flat layout
        };
        for (const auto& c : candidates) {
            if (fs::exists(c, ec)) {
                const fs::path canon = fs::weakly_canonical(c, ec);
                return (ec ? c : canon).string();
            }
        }
    }
    return bakedAbs ? std::string(bakedAbs) : std::string();
}

} // namespace liminal::editor
