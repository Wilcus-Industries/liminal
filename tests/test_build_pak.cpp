// test_build_pak — exercises the editor "Build Game" pak-assembly path headless.
// Builds editor/sample_project's pak via the shared buildGamePak() helper,
// appends it to a fresh (empty) host file (sidecar-style), then re-opens it with
// PakReader and asserts the normalized project.ljson and the startup scene are
// present and parseable. It does NOT launch the GUI player (no display in CI).

#include <liminal/core/pak.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace liminal;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++g_failures; }
}
} // namespace

int main() {
    // Locate sample_project relative to this source file's known repo layout.
    // LIMINAL_SAMPLE_PROJECT_DIR is baked by tests/CMakeLists.txt.
#ifndef LIMINAL_SAMPLE_PROJECT_DIR
#error "LIMINAL_SAMPLE_PROJECT_DIR must be defined by the build"
#endif
    const fs::path projDir = LIMINAL_SAMPLE_PROJECT_DIR;
    const fs::path projFile = projDir / "project.ljson";
    check(fs::exists(projFile), "sample project.ljson exists");

    // Read project.ljson the way openProject does: assetRoot relative to it.
    nlohmann::json pj;
    { std::ifstream in(projFile); in >> pj; }
    const std::string assetRoot =
        (projDir / pj.value("assetRoot", ".")).lexically_normal().string();
    const std::string startup = pj.value("startupScene", "");
    const std::string title = pj.value("title", projDir.filename().string());

    PakWriter w;
    const bool ok = buildGamePak(assetRoot, startup, title, w, std::string(),
                                 [](const std::string& m) {
                                     std::printf("%s\n", m.c_str());
                                 });
    check(ok, "buildGamePak succeeds for sample_project");

    // Append to a fresh empty host (mirrors the macOS sidecar path).
    const fs::path host = fs::temp_directory_path() / "liminal_build_pak_test.pak";
    { std::ofstream(host.string(), std::ios::binary | std::ios::trunc); }
    check(w.appendTo(host.string()), "appendTo fresh host succeeds");

    PakReader r;
    check(r.open(host.string()), "PakReader opens the built pak");

    // Exactly one project.ljson, at the pak root, parseable, normalized.
    check(r.contains("project.ljson"), "pak contains root project.ljson");
    auto pjTxt = r.read("project.ljson");
    check(pjTxt.has_value(), "project.ljson reads back");
    if (pjTxt) {
        nlohmann::json out = nlohmann::json::parse(*pjTxt, nullptr, false);
        check(!out.is_discarded(), "project.ljson parses");
        check(out.value("assetRoot", "") == ".",
              "project.ljson assetRoot normalized to '.'");
        check(out.value("startupScene", "") == startup,
              "project.ljson startupScene preserved (relative to assetRoot)");
    }

    // The startup scene itself must be in the pak.
    check(!startup.empty() && r.contains(startup),
          "pak contains the startup scene");

    // A script that ships with the sample must be present too.
    check(r.contains("scripts/spin.lua"), "pak contains scripts/spin.lua");

    std::error_code ec;
    fs::remove(host, ec);

    if (g_failures == 0) { std::printf("test_build_pak: OK\n"); return 0; }
    std::fprintf(stderr, "test_build_pak: %d failure(s)\n", g_failures);
    return 1;
}
