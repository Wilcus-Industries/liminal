#pragma once
// Recent-projects persistence for the editor landing screen. A small JSON array
// stored at liminal::userConfigDir()/recent_projects.json, most-recent-first.
// Entries point at a project.ljson (absolute path). Independent of EditorApp so
// the landing UI and openProject can share it without pulling in editor state.

#include <string>
#include <vector>

namespace liminal::editor {

struct RecentProject {
    std::string path;        // absolute path to project.ljson
    std::string title;       // display title (project folder name or title key)
    long long lastOpened = 0; // unix seconds; sort key (desc)
};

// Reads the recents file, newest first. Missing/corrupt file → empty vector
// (never throws). Entries are returned as-stored; callers may check fs::exists
// on each path to grey out projects whose directory has since moved/deleted.
std::vector<RecentProject> loadRecentProjects();

// Records (or refreshes) a project at the front of the list: canonicalizes the
// path, de-dupes by path, stamps lastOpened = now, caps the list (~15), and
// writes it back. Best-effort and non-fatal — write failures are silent (the
// recents list is convenience state, never load-bearing).
void recordRecentProject(const std::string& projectFile, const std::string& title);

// Drops a project (matched by path, canonicalized) from the list and rewrites
// it. Used by the landing screen's "remove from list" affordance.
void removeRecentProject(const std::string& projectFile);

} // namespace liminal::editor
