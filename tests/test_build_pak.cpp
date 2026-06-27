// test_build_pak — exercises the editor "Build Game" pak-assembly path headless.
// Synthesizes a throwaway project (project.ljson + one scene + one script) in a
// temp dir, builds its pak via the shared buildGamePak() helper, appends it to a
// fresh (empty) host file (sidecar-style), then re-opens it with PakReader and
// asserts the normalized project.ljson and the startup scene are present and
// parseable. It does NOT launch the GUI player (no display in CI).

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

void writeFile(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << contents;
}
} // namespace

int main() {
    // Synthesize a minimal project on disk (no checked-in fixture needed).
    const fs::path projDir =
        fs::temp_directory_path() / "liminal_build_pak_proj";
    fs::remove_all(projDir);
    fs::create_directories(projDir);

    const std::string startup = "scenes/main.lscene";
    const std::string script = "scripts/spin.lua";
    const std::string title = "paktest";

    writeFile(projDir / "project.ljson",
              "{\n"
              "  \"title\": \"" + title + "\",\n"
              "  \"assetRoot\": \".\",\n"
              "  \"startupScene\": \"" + startup + "\"\n"
              "}\n");
    writeFile(projDir / startup, "{\"liminal_scene\":1,\"entities\":[]}\n");
    writeFile(projDir / script,
              "function on_update(self, dt) end\n");

    const fs::path projFile = projDir / "project.ljson";
    check(fs::exists(projFile), "synthesized project.ljson exists");

    // Read project.ljson the way openProject does: assetRoot relative to it.
    nlohmann::json pj;
    { std::ifstream in(projFile); in >> pj; }
    const std::string assetRoot =
        (projDir / pj.value("assetRoot", ".")).lexically_normal().string();
    const std::string startupScene = pj.value("startupScene", "");

    PakWriter w;
    const bool ok = buildGamePak(assetRoot, startupScene,
                                 pj.value("title", title), w, std::string(),
                                 [](const std::string& m) {
                                     std::printf("%s\n", m.c_str());
                                 });
    check(ok, "buildGamePak succeeds for the synthesized project");

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
        check(out.value("startupScene", "") == startupScene,
              "project.ljson startupScene preserved (relative to assetRoot)");
    }

    // The startup scene itself must be in the pak.
    check(!startupScene.empty() && r.contains(startupScene),
          "pak contains the startup scene");

    // The script that ships with the project must be present too.
    check(r.contains(script), "pak contains scripts/spin.lua");

    std::error_code ec;
    fs::remove(host, ec);
    fs::remove_all(projDir, ec);

    if (g_failures == 0) { std::printf("test_build_pak: OK\n"); return 0; }
    std::fprintf(stderr, "test_build_pak: %d failure(s)\n", g_failures);
    return 1;
}
