// test_pak_roundtrip — pak write/read byte-fidelity, corruption handling, and
// the Assets VFS mount (pak hit + disk fallback). No GL, no scripting needed.

#include <liminal/core/assets.hpp>
#include <liminal/core/pak.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace liminal;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// Write raw bytes to a path (binary), overwriting.
void writeFile(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path();
    const fs::path host = tmp / "liminal_pak_test_host.bin";
    const fs::path diskAsset = tmp / "liminal_pak_test_disk.txt";

    // --- dummy host file with arbitrary leading bytes (incl. NULs) ----------
    std::string host0 = "MZ host program bytes here";
    host0.append(std::string("\x00\x00binary\xFF\xFE", 9));
    writeFile(host, host0);

    // open() on a non-pak file must fail cleanly (no throw).
    {
        PakReader r;
        check(!r.open(host.string()), "open() on bare host file returns false");
        check(!r.open((tmp / "definitely_missing_file_xyz.bin").string()),
              "open() on missing file returns false");
    }

    // --- build a pak with varied entries ------------------------------------
    const std::string txtPlain = "hello pak\n";
    std::string binWithNul;
    binWithNul.append(std::string("\x00\x01\x02\x03\xFF\x00\xAB", 7));
    const std::string empty = "";
    const std::string nested = "deeply nested content 12345";

    {
        PakWriter w;
        w.add("greeting.txt", txtPlain);
        w.add("a/b/c.txt", nested);
        w.add("blob.bin", binWithNul);
        w.add("empty.dat", empty);
        check(w.appendTo(host.string()), "appendTo() succeeds");
    }

    // Host bytes must be intact at the front.
    {
        std::ifstream in(host, std::ios::binary);
        std::string head(host0.size(), '\0');
        in.read(head.data(), static_cast<std::streamsize>(host0.size()));
        check(head == host0, "host bytes untouched after append");
    }

    // --- read back ----------------------------------------------------------
    auto pak = std::make_shared<PakReader>();
    check(pak->open(host.string()), "open() on appended pak returns true");

    check(pak->contains("greeting.txt"), "contains greeting.txt");
    check(pak->contains("a/b/c.txt"), "contains nested");
    check(pak->contains("./greeting.txt"), "contains normalizes ./ prefix");
    check(!pak->contains("missing.txt"), "missing path not contained");

    auto r1 = pak->read("greeting.txt");
    check(r1 && *r1 == txtPlain, "greeting bytes exact");
    auto r2 = pak->read("a/b/c.txt");
    check(r2 && *r2 == nested, "nested bytes exact");
    auto r3 = pak->read("blob.bin");
    check(r3 && *r3 == binWithNul, "binary-with-NUL bytes exact");
    auto r4 = pak->read("empty.dat");
    check(r4 && r4->empty(), "empty entry reads as empty string");
    auto r5 = pak->read("nope.txt");
    check(!r5, "missing path read() returns nullopt");

    // paths() lists every entry.
    {
        auto ps = pak->paths();
        check(ps.size() == 4, "paths() lists all four entries");
    }

    // --- Assets VFS: pak hit + disk fallback --------------------------------
    writeFile(diskAsset, "on disk only");
    Assets::addSearchPath(tmp.string());
    Assets::mountPak(pak);
    check(Assets::hasPak(), "Assets::hasPak() true after mount");

    auto fromPak = Assets::readFile("greeting.txt");
    check(fromPak && *fromPak == txtPlain, "Assets::readFile serves pak entry");

    auto fromDisk = Assets::readFile("liminal_pak_test_disk.txt");
    check(fromDisk && *fromDisk == "on disk only",
          "Assets::readFile falls back to disk for non-pak path");

    auto nope = Assets::readFile("not/anywhere.txt");
    check(!nope, "Assets::readFile nullopt when neither pak nor disk has it");

    Assets::mountPak(nullptr);
    check(!Assets::hasPak(), "Assets::hasPak() false after unmount");

    // --- cleanup ------------------------------------------------------------
    std::error_code ec;
    fs::remove(host, ec);
    fs::remove(diskAsset, ec);

    if (g_failures == 0) {
        std::printf("test_pak_roundtrip: OK\n");
        return 0;
    }
    std::fprintf(stderr, "test_pak_roundtrip: %d failure(s)\n", g_failures);
    return 1;
}
