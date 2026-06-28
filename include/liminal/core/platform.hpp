#pragma once
// Cross-platform process introspection. selfExePath() returns the absolute,
// symlink-resolved path of the running executable as a std::filesystem-friendly
// UTF-8 string; selfExeDir() is its parent directory. Used by the standalone
// player (to find a pak appended to / beside itself) and the editor.

#include <filesystem>
#include <string>

namespace liminal {

// Absolute path to the running executable (UTF-8). Empty string on failure.
std::string selfExePath();

// Parent directory of selfExePath() (UTF-8). Empty string on failure.
std::string selfExeDir();

// Open a URL (or file path) in the OS default handler — macOS `open`, Linux
// `xdg-open`, Windows ShellExecute. Best-effort and non-fatal; returns true if
// the launch was issued (not whether the handler ultimately succeeded).
bool openUrl(const std::string& url);

// Per-user config directory for liminal: ~/.liminal (POSIX, via $HOME) or
// %APPDATA%/liminal (Windows). Created on first use. Empty path on failure
// (e.g. no $HOME). Used for editor-level state like the recent-projects list.
std::filesystem::path userConfigDir();

// The user's home directory: $HOME (POSIX) / %USERPROFILE% (Windows). NOT
// created (it already exists); empty path on failure. Used by the editor to
// install the agent bootstrap skill under ~/.claude, ~/.codex, etc.
std::filesystem::path userHomeDir();

} // namespace liminal
