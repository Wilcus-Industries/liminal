// pak.cpp — see <liminal/core/pak.hpp> for the format. Everything here is
// byte-level and endian-explicit on purpose: the same archive must read
// identically on macOS, Windows, and Linux regardless of native endianness or
// struct padding, so we never memcpy a struct and never rely on host int order.

#include <liminal/core/pak.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>

namespace liminal {

namespace fs = std::filesystem;

namespace {

// --- explicit little-endian (de)serialization ----------------------------
// Writers append to a std::string buffer; readers pull from a byte span with
// a moving cursor and a bounds flag. No host-endian assumptions anywhere.

void putU16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void putU32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putU64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::uint16_t getU16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t getU32(const unsigned char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

std::uint64_t getU64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

} // namespace

std::string pakNormalize(const std::string& relpath) {
    std::string s;
    s.reserve(relpath.size());
    for (char c : relpath) s.push_back(c == '\\' ? '/' : c);
    // Strip leading "./" segments and any leading '/'.
    std::size_t i = 0;
    for (;;) {
        if (i + 1 < s.size() && s[i] == '.' && s[i + 1] == '/') {
            i += 2;
        } else if (i < s.size() && s[i] == '/') {
            i += 1;
        } else {
            break;
        }
    }
    return s.substr(i);
}

// ---------------------------------------------------------------------------
// PakWriter
// ---------------------------------------------------------------------------

void PakWriter::add(const std::string& relpath, std::string bytes) {
    std::string path = pakNormalize(relpath);
    // The index stores path length as a u16; a path longer than 0xFFFF bytes
    // would truncate and corrupt the archive. Drop it (and don't count it) so
    // the written file-count stays consistent with the entries.
    if (path.size() > 0xFFFF) {
        std::fprintf(stderr,
                     "[pak] path exceeds 65535 bytes, skipped: %.64s...\n",
                     path.c_str());
        return;
    }
    m_files.push_back({std::move(path), std::move(bytes)});
}

bool PakWriter::appendTo(const std::string& hostFilePath) {
    // Determine the pak start (current host size). Open read-binary to measure;
    // the host may not exist yet, which is a failure — we append, not create.
    std::uint64_t pakStart = 0;
    {
        std::ifstream probe(hostFilePath, std::ios::binary | std::ios::ate);
        if (!probe) return false;
        const std::streamoff end = probe.tellg();
        if (end < 0) return false;
        pakStart = static_cast<std::uint64_t>(end);
    }

    // Build the blob and index in memory (offsets relative to pakStart).
    std::string blob;
    std::string index;
    putU32(index, static_cast<std::uint32_t>(m_files.size()));

    std::uint64_t cursor = 0; // offset within the blob == offset rel to pakStart
    for (const auto& f : m_files) {
        const std::uint64_t offset = cursor;
        blob.append(f.bytes);
        cursor += f.bytes.size();

        putU16(index, static_cast<std::uint16_t>(f.path.size()));
        index.append(f.path);
        putU64(index, offset);
        putU64(index, static_cast<std::uint64_t>(f.bytes.size()));
    }

    const std::uint64_t indexAbs = pakStart + blob.size();

    std::string footer;
    putU64(footer, pakStart);
    putU64(footer, indexAbs);
    footer.append(kPakMagic, sizeof(kPakMagic));

    std::ofstream out(hostFilePath, std::ios::binary | std::ios::app);
    if (!out) return false;
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    out.write(index.data(), static_cast<std::streamsize>(index.size()));
    out.write(footer.data(), static_cast<std::streamsize>(footer.size()));
    out.flush();
    return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------
// buildGamePak — project asset collection + normalized project.ljson
// ---------------------------------------------------------------------------

namespace {

std::string toLowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

// Whitelisted shippable extensions (lowercase, with leading dot).
bool isShippableExt(const std::string& extLower) {
    static const char* kExts[] = {
        ".lscene", ".lua",  ".ljson", ".json", ".glsl", ".vert", ".frag",
        ".txt",    ".png",  ".jpg",   ".jpeg", ".tga",  ".bmp"};
    for (const char* e : kExts)
        if (extLower == e) return true;
    return false;
}

// True if any directory segment of `rel` starts with '.' (dotfiles/dotdirs).
bool hasDotSegment(const fs::path& rel) {
    for (const auto& seg : rel) {
        const std::string s = seg.string();
        if (!s.empty() && s[0] == '.' && s != "." && s != "..") return true;
    }
    return false;
}

} // namespace

bool buildGamePak(const std::string& assetRoot, const std::string& startupScene,
                  const std::string& title, PakWriter& out,
                  const std::string& skipDir,
                  const std::function<void(const std::string&)>& log) {
    auto note = [&](const std::string& m) { if (log) log(m); };

    std::error_code ec;
    const fs::path root = fs::absolute(assetRoot, ec).lexically_normal();
    if (ec || !fs::is_directory(root, ec)) {
        note("[build] asset root is not a directory: " + assetRoot);
        return false;
    }
    const fs::path skip =
        skipDir.empty() ? fs::path() : fs::absolute(skipDir, ec).lexically_normal();

    constexpr std::uintmax_t kSizeCap = 256ull * 1024 * 1024;

    int added = 0;
    for (fs::recursive_directory_iterator it(
             root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { note("[build] walk error: " + ec.message()); ec.clear(); continue; }
        const fs::path& abs = it->path();

        // Skip the build-output subtree entirely (avoids packing prior builds).
        if (!skip.empty()) {
            const fs::path a = abs.lexically_normal();
            auto rs = a.lexically_relative(skip);
            if (!rs.empty() && *rs.begin() != "..") {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
        }

        fs::path rel = abs.lexically_relative(root);
        if (rel.empty()) continue;
        if (hasDotSegment(rel)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;

        const std::string ext = toLowerExt(abs);
        if (ext == ".gguf") continue;            // models ship beside the exe
        if (!isShippableExt(ext)) continue;

        const std::string relKey = rel.generic_string();
        if (relKey == "project.ljson") continue; // synthesized below, once

        const std::uintmax_t sz = fs::file_size(abs, ec);
        if (ec) { note("[build] cannot stat, skipped: " + relKey); ec.clear(); continue; }
        if (sz > kSizeCap) {
            note("[build] file exceeds 256 MB cap, skipped: " + relKey);
            continue;
        }

        std::ifstream in(abs, std::ios::binary);
        if (!in) { note("[build] cannot read, skipped: " + relKey); continue; }
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        out.add(relKey, std::move(bytes));
        ++added;
    }

    // Synthesize the pak-root project.ljson: assetRoot ".", startupScene made
    // relative to assetRoot (so it resolves directly through the mounted pak).
    std::string startup = startupScene;
    if (!startup.empty()) {
        const fs::path sp = fs::path(startup);
        if (sp.is_absolute()) {
            fs::path r = fs::absolute(sp, ec).lexically_normal()
                             .lexically_relative(root);
            if (!r.empty() && *r.begin() != "..") startup = r.generic_string();
        } else {
            startup = sp.lexically_normal().generic_string();
        }
    }
    nlohmann::json pj;
    pj["assetRoot"] = ".";
    if (!startup.empty()) pj["startupScene"] = startup;
    pj["title"] = title.empty() ? std::string("liminal") : title;
    // Carry through the window size from the source project.ljson if it set one,
    // so the player can honor it (it reads width/height; absent = its defaults).
    {
        std::ifstream pin(root / "project.ljson", std::ios::binary);
        if (pin) {
            std::string ptxt((std::istreambuf_iterator<char>(pin)),
                             std::istreambuf_iterator<char>());
            try {
                const nlohmann::json src = nlohmann::json::parse(ptxt);
                if (src.contains("width"))  pj["width"]  = src["width"].get<int>();
                if (src.contains("height")) pj["height"] = src["height"].get<int>();
            } catch (const std::exception&) {
                // Malformed source project.ljson: just omit the size fields.
            }
        }
    }
    out.add("project.ljson", pj.dump(4));

    note("[build] packed " + std::to_string(added) +
         " asset file(s) + project.ljson (startupScene: " +
         (startup.empty() ? "<none>" : startup) + ")");
    return true;
}

// ---------------------------------------------------------------------------
// PakReader
// ---------------------------------------------------------------------------

bool PakReader::open(const std::string& filePath) {
    m_entries.clear();
    m_order.clear();
    m_pakStart = 0;
    m_filePath.clear();

    std::ifstream in(filePath, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff endOff = in.tellg();
    if (endOff < kPakFooterSize) return false;
    const std::uint64_t fileSize = static_cast<std::uint64_t>(endOff);

    // Read the 24-byte footer.
    unsigned char footer[kPakFooterSize];
    in.seekg(static_cast<std::streamoff>(fileSize - kPakFooterSize), std::ios::beg);
    in.read(reinterpret_cast<char*>(footer), kPakFooterSize);
    if (!in || in.gcount() != kPakFooterSize) return false;

    // Verify magic (bytes 16..23).
    for (int i = 0; i < 8; ++i) {
        if (footer[16 + i] != static_cast<unsigned char>(kPakMagic[i])) return false;
    }

    const std::uint64_t pakStart = getU64(footer);
    const std::uint64_t indexAbs = getU64(footer + 8);

    // Sanity: index must sit between pakStart and the footer.
    if (pakStart > indexAbs) return false;
    if (indexAbs > fileSize - kPakFooterSize) return false;

    // Read the index region (everything from indexAbs up to the footer).
    const std::uint64_t indexLen = (fileSize - kPakFooterSize) - indexAbs;
    if (indexLen < 4) return false; // needs at least the u32 count
    std::string idx;
    idx.resize(static_cast<std::size_t>(indexLen));
    in.seekg(static_cast<std::streamoff>(indexAbs), std::ios::beg);
    in.read(idx.data(), static_cast<std::streamsize>(indexLen));
    if (!in || static_cast<std::uint64_t>(in.gcount()) != indexLen) return false;

    const auto* p = reinterpret_cast<const unsigned char*>(idx.data());
    std::size_t pos = 0;
    const std::size_t n = idx.size();
    auto need = [&](std::size_t bytes) { return pos + bytes <= n; };

    if (!need(4)) return false;
    const std::uint32_t count = getU32(p + pos);
    pos += 4;

    const std::uint64_t blobLimit = indexAbs - pakStart; // max valid rel offset
    for (std::uint32_t e = 0; e < count; ++e) {
        if (!need(2)) return false;
        const std::uint16_t pathLen = getU16(p + pos);
        pos += 2;
        if (!need(pathLen)) return false;
        std::string path(reinterpret_cast<const char*>(p + pos), pathLen);
        pos += pathLen;
        if (!need(16)) return false;
        Entry ent;
        ent.offset = getU64(p + pos);
        ent.size = getU64(p + pos + 8);
        pos += 16;

        // Bounds-check the data range against the blob region.
        if (ent.offset > blobLimit || ent.size > blobLimit - ent.offset) {
            return false;
        }
        const std::string key = pakNormalize(path);
        if (m_entries.emplace(key, ent).second) m_order.push_back(key);
    }

    m_filePath = filePath;
    m_pakStart = pakStart;
    return true;
}

bool PakReader::contains(const std::string& relpath) const {
    return m_entries.count(pakNormalize(relpath)) != 0;
}

std::optional<std::string> PakReader::read(const std::string& relpath) const {
    auto it = m_entries.find(pakNormalize(relpath));
    if (it == m_entries.end()) return std::nullopt;
    const Entry& ent = it->second;

    std::ifstream in(m_filePath, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(static_cast<std::streamoff>(m_pakStart + ent.offset), std::ios::beg);
    if (!in) return std::nullopt;

    std::string out;
    out.resize(static_cast<std::size_t>(ent.size));
    if (ent.size > 0) {
        in.read(out.data(), static_cast<std::streamsize>(ent.size));
        if (!in || static_cast<std::uint64_t>(in.gcount()) != ent.size) {
            return std::nullopt;
        }
    }
    return out;
}

std::vector<std::string> PakReader::paths() const {
    return m_order;
}

} // namespace liminal
