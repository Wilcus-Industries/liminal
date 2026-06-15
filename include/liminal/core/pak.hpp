#pragma once
// Pak archive — a flat read-only bundle of project assets appended to a host
// binary so a standalone player can serve everything from its own executable.
//
// Layout (all integers little-endian, written byte-by-byte; never memcpy'd
// structs, so the format is identical on every platform/compiler):
//
//   [host bytes ...]          the original executable, untouched
//   [file data blob]          every added file's bytes, concatenated
//   [index]                   u32 count, then per entry:
//                               u16 pathLen, path bytes (UTF-8, '/'-sep, rel),
//                               u64 offset (relative to pak start),
//                               u64 size
//   [footer]                  the last 24 bytes of the file:
//                               u64 pakStartAbsOffset,
//                               u64 indexAbsOffset,
//                               char magic[8] = "LMPK\0v01"
//
// "pak start" is the absolute file offset where our appended blob begins (i.e.
// the original host size). All in-index offsets are relative to it, so the pak
// is position-independent: data offset N lives at pakStart + N in the file.
//
// Reading is robust: any truncation/corruption (short file, missing/garbled
// magic, out-of-range offsets) makes PakReader::open return false rather than
// throw. Paths are normalized on both write and read — forward slashes, no
// leading "./" — so lookups match regardless of how the caller spells them.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace liminal {

// The 8-byte tail magic. The embedded NUL is deliberate (binary marker), so
// it's spelled out rather than written as a C string literal.
inline constexpr char kPakMagic[8] = {'L', 'M', 'P', 'K', '\0', 'v', '0', '1'};
inline constexpr int kPakFooterSize = 24; // 8 + 8 + 8

// Normalize a relative path to the canonical key form pak entries use:
// backslashes → '/', collapse "./" prefixes, drop a leading '/'. Pure string
// work (no filesystem touch) so it's identical on every platform.
std::string pakNormalize(const std::string& relpath);

class PakWriter {
public:
    // Queue a file. `relpath` is stored normalized ('/'-sep, relative).
    // `bytes` may contain NULs / be empty; it's taken by value (moved in).
    void add(const std::string& relpath, std::string bytes);

    // Append [blob][index][footer] to the file at `hostFilePath` (opened for
    // binary read+append). Returns false on any I/O failure. The host's
    // original bytes are left untouched; pakStart is the pre-append file size.
    bool appendTo(const std::string& hostFilePath);

private:
    struct Pending {
        std::string path;  // normalized key
        std::string bytes; // file contents
    };
    std::vector<Pending> m_files;
};

// Collect a project's shippable assets into `out` and inject a normalized,
// pak-root `project.ljson` (assetRoot ".", startupScene made relative to
// assetRoot, plus the title). Walks `assetRoot` recursively, including only the
// known asset extensions and skipping dotfiles/dirs, .gguf models (they ship
// beside the exe), the on-disk project.ljson (the synthesized one replaces it),
// anything under `skipDir` (e.g. the build output), and files past a size cap.
//
// Pure filesystem + string work — no GL, no platform-specific calls — so both
// the editor's "Build Game" command and the headless tests share one code path.
// `log` receives human-readable progress/warning lines (may be null). Returns
// false only on a hard failure (assetRoot missing / unreadable); per-file
// problems are logged and skipped.
bool buildGamePak(const std::string& assetRoot,
                  const std::string& startupScene,
                  const std::string& title,
                  PakWriter& out,
                  const std::string& skipDir = std::string(),
                  const std::function<void(const std::string&)>& log = nullptr);

class PakReader {
public:
    // Open `filePath`, read the 24-byte footer, verify magic, and load the
    // index into memory. Returns false (no throw) if the file is missing,
    // too short, lacks valid magic, or the index is inconsistent.
    bool open(const std::string& filePath);

    bool contains(const std::string& relpath) const;

    // The bytes of `relpath`, or nullopt if absent / the read fails. Seeks to
    // pakStart + entry.offset and reads entry.size bytes.
    std::optional<std::string> read(const std::string& relpath) const;

    // Every stored path (normalized), in index order.
    std::vector<std::string> paths() const;

private:
    struct Entry {
        std::uint64_t offset = 0; // relative to m_pakStart
        std::uint64_t size = 0;
    };
    std::string m_filePath;
    std::uint64_t m_pakStart = 0;
    std::map<std::string, Entry> m_entries;
    std::vector<std::string> m_order; // paths in index order
};

} // namespace liminal
