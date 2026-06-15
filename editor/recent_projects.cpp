#include "recent_projects.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include <liminal/core/platform.hpp>

namespace liminal::editor {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxRecents = 15;

fs::path recentsFile() {
    const fs::path dir = userConfigDir();
    if (dir.empty()) return {};
    return dir / "recent_projects.json";
}

// Canonicalize a project path for stable de-dup. Falls back to lexically_normal
// absolute when the file no longer exists (weakly_canonical handles that too,
// but guard against exceptions on odd inputs).
std::string canonical(const std::string& p) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
    if (ec) abs = fs::absolute(p, ec);
    return abs.lexically_normal().string();
}

} // namespace

std::vector<RecentProject> loadRecentProjects() {
    std::vector<RecentProject> out;
    const fs::path file = recentsFile();
    if (file.empty()) return out;
    std::ifstream in(file);
    if (!in) return out;
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return out; // corrupt file → treat as empty
    }
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        RecentProject rp;
        rp.path = e.value("path", std::string{});
        if (rp.path.empty()) continue;
        rp.title = e.value("title", std::string{});
        rp.lastOpened = e.value("lastOpened", 0LL);
        out.push_back(std::move(rp));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const RecentProject& a, const RecentProject& b) {
                         return a.lastOpened > b.lastOpened;
                     });
    return out;
}

static void writeRecents(const std::vector<RecentProject>& list) {
    const fs::path file = recentsFile();
    if (file.empty()) return;
    nlohmann::json j = nlohmann::json::array();
    for (const auto& rp : list)
        j.push_back({{"path", rp.path},
                     {"title", rp.title},
                     {"lastOpened", rp.lastOpened}});
    std::ofstream out(file);
    if (!out) return;
    out << j.dump(2) << '\n';
}

void recordRecentProject(const std::string& projectFile,
                         const std::string& title) {
    if (projectFile.empty()) return;
    const std::string key = canonical(projectFile);
    std::vector<RecentProject> list = loadRecentProjects();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const RecentProject& rp) {
                                  return canonical(rp.path) == key;
                              }),
               list.end());
    RecentProject rp;
    rp.path = key;
    rp.title = title;
    rp.lastOpened = static_cast<long long>(std::time(nullptr));
    list.insert(list.begin(), std::move(rp));
    if (list.size() > kMaxRecents) list.resize(kMaxRecents);
    writeRecents(list);
}

void removeRecentProject(const std::string& projectFile) {
    if (projectFile.empty()) return;
    const std::string key = canonical(projectFile);
    std::vector<RecentProject> list = loadRecentProjects();
    const std::size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const RecentProject& rp) {
                                  return canonical(rp.path) == key;
                              }),
               list.end());
    if (list.size() != before) writeRecents(list);
}

} // namespace liminal::editor
