#pragma once
// Editor self-update check. On the first project open we kick a one-shot
// background probe of the project's GitHub repo for the latest STABLE release
// and, if the running editor is behind, the menubar shows a red warning.
//
// There is no HTTPS client in the tree (cpp-httplib is built plaintext-only,
// no curl/libcurl linked), so the fetch shells out to the system `curl` —
// the same std::system idiom platform::openUrl uses. Any failure (no curl,
// offline, no published release, parse error) resolves to "no warning": the
// feature is convenience state, never load-bearing.

#include <atomic>
#include <mutex>
#include <string>

namespace liminal::editor {

// Shared between EditorApp and the detached worker thread. Held by shared_ptr
// so the worker stays safe if EditorApp is destroyed before the probe returns.
struct UpdateCheckState {
    enum class Status { Checking, UpToDate, OutOfDate, Failed };
    std::atomic<Status> status{Status::Checking};
    std::mutex mtx;        // guards latestTag
    std::string latestTag; // GitHub tag_name (e.g. "v0.3.0"); set when OutOfDate
};

// Parse "1.2.3" / "v1.2.3" (tolerates a trailing "-rc1" / "+build" on patch).
// Returns false on malformed input; maj/min/patch only valid when true.
bool parseSemver(const std::string& s, int& maj, int& min, int& patch);

// True when `latest` is a strictly newer semver than `current`. False if either
// fails to parse (so a junk tag never produces a spurious warning).
bool isNewer(const std::string& latest, const std::string& current);

// Blocking. Shells `curl` to the GitHub releases/latest API and returns the
// release tag_name, or "" on ANY failure. Safe to call off the main thread.
std::string fetchLatestReleaseTag();

} // namespace liminal::editor
