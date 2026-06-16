#pragma once
// ScriptHost: Lua behavior for Script components. One sol::state for the
// whole host; each Script-component instance runs its chunk inside its own
// sol::environment (globals fall through, writes stay local), so two
// entities sharing spin.lua never share state.
//
// Script file contract — the file RETURNS a table:
//
//   local M = {}
//   function M.on_start(self)       end  -- once, when first seen / reloaded
//   function M.on_update(self, dt)  end  -- every frame
//   return M
//
// `self` is the lm Entity userdata for the owning entity. Both functions are
// optional. An entity's Script.paths may list multiple scripts; each runs as
// an independent instance (own environment, own on_start/on_update). Paths are
// resolved through Assets::resolve, so apps register their script dirs as
// asset search paths.
//
// update(scene, dt) drives the lifecycle: lazily inits newly-seen Script
// components (on_start once), ticks on_update, and drops state for entities
// that were destroyed or lost their Script component. All Lua entry points
// are pcall'd; an error prints "[lua] path: message" ONCE per distinct
// message (no per-frame spam) and the affected instance is parked until the
// file changes.
//
// Hot reload: every 0.5s of accumulated update() time the host stats each
// known script file; an mtime change re-reads the source and rebuilds every
// instance of that script from scratch (fresh environment, on_start re-runs).
//
// Libraries: base/math/string/table/os/io are all opened (full standard-
// library file access; io/os read the real filesystem, not the mounted pak).
// The lm.* API surface is bound in src/script/lua_bindings.cpp.

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>
#include <sol/sol.hpp>

namespace liminal {

class Scene;
class Window;
class Audio;
class AssetCache;
class Renderer;
#if defined(LIMINAL_WITH_INFERENCE)
namespace inference { class Engine; }
#endif

// The host's view of the surrounding app: which subsystems the lm.* API may
// reach. All pointers may be null — every binding null-guards (setters no-op,
// getters return defaults, with a once-per-session warning). App and the
// editor each build their own context.
struct ScriptContext {
    Window* input = nullptr;        // lm.input.key_down
    Audio* audio = nullptr;         // lm.audio.*
    AssetCache* assets = nullptr;   // lm.assets.*
    Renderer* renderer = nullptr;   // lm.ui / lm.render (plumbing; no API yet)
#if defined(LIMINAL_WITH_INFERENCE)
    inference::Engine* inference = nullptr; // wired in a later chunk; field now
#endif
    bool hotReload = true;          // pollReloads() is a no-op when false
    // Deferred scene switch for lm.scene.change(path). Null = unsupported
    // (lm.scene.change then warns once). App stores a pending path and swaps
    // at end of frame; the editor logs that Play doesn't support it.
    std::function<void(const std::string&)> requestSceneChange;
};

class ScriptHost {
public:
    // The context names the subsystems the lm.* API may touch; see
    // ScriptContext. The default {} is a fully windowless/subsystem-less host
    // (input false, audio/assets no-op) — handy for tests.
    explicit ScriptHost(ScriptContext ctx = {});
    ~ScriptHost();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Drives every Script component in `scene`. Call once per frame.
    void update(Scene& scene, float dt);

    // Escape hatch: the raw state, for apps that want to bind extra API.
    sol::state& lua() { return m_lua; }

    // The subsystem context this host was built with (lm.* bindings close
    // over it through the host).
    const ScriptContext& context() const { return m_ctx; }

    // Valid during update() (lm.scene closes over the host); null outside.
    Scene* activeScene() { return m_scene; }
    Window* input() { return m_ctx.input; }
    // Seconds since host construction — what lm.time.now() returns.
    double now() const;

    // Optional sink for script errors, called with "path: message" alongside
    // the stderr print (same once-per-distinct-message dedup). The editor
    // uses this to route [lua] errors into its console. Null disables.
    void setErrorSink(std::function<void(const std::string&)> sink) {
        m_errorSink = std::move(sink);
    }

    // Optional sink for lm.log output, called with the raw message alongside
    // the stdout print. The editor uses this to route [lua] log lines into its
    // console (otherwise they only reach the terminal). Null disables.
    void setLogSink(std::function<void(const std::string&)> sink) {
        m_logSink = std::move(sink);
    }
    // Emit an lm.log line: always prints to stdout, plus the log sink if set.
    void emitLog(const std::string& msg);

    // lm.import(path): project-relative, VFS/pak-aware module loader. Loads the
    // Lua file once (read pak-first via Assets::readFile), caches its return
    // value, and hands the SAME object to every importer — shared state across
    // scripts. A chunk that returns nothing caches `true` (require convention,
    // so it isn't reloaded). On read/load/run error the error is reported and
    // lua_nil is returned; importModule never throws out to the caller. Cyclic
    // imports of the same path return the in-flight placeholder (nil) instead of
    // looping. Cached for the host's lifetime (no hot reload).
    sol::object importModule(const std::string& path);

private:
    struct ScriptFile {
        std::string resolved;                    // Assets::resolve result
        std::string source;                      // file contents
        std::filesystem::file_time_type mtime{}; // for hot reload (disk only)
        bool ok = false;                         // file read successfully
        bool fromPak = false;                    // served from a mounted pak —
                                                 // no mtime; pollReloads skips
    };
    struct Instance {
        std::string path;     // Script.path as authored (file table key)
        sol::environment env; // per-entity sandbox
        sol::table table;     // the table the chunk returned
        bool started = false; // on_start already ran
        bool failed = false;  // parked until the file is touched
    };

    ScriptFile& ensureFile(const std::string& path);
    void initInstance(Instance& inst, entt::entity e, Scene& scene);
    void pollReloads();
    void reportError(const std::string& path, const std::string& message);

    sol::state m_lua;
    ScriptContext m_ctx;
    Scene* m_scene = nullptr;
    std::unordered_map<std::string, ScriptFile> m_files;      // by script path
    std::unordered_map<std::string, sol::object> m_modules;   // lm.import cache
    // One entity can run many scripts: each entity maps to a vector of
    // Instances, one per path in its Script.paths.
    std::unordered_map<entt::entity, std::vector<Instance>> m_instances;
    std::unordered_set<std::string> m_reportedErrors;
    std::function<void(const std::string&)> m_errorSink;
    std::function<void(const std::string&)> m_logSink;
    float m_reloadTimer = 0.0f;
    std::chrono::steady_clock::time_point m_epoch;
};

} // namespace liminal
