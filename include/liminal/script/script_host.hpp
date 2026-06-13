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
// optional. Paths in Script{path} are resolved through Assets::resolve, so
// apps register their script dirs as asset search paths.
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
// Sandbox: base/math/string/table libraries plus a pruned os table (time,
// clock, date, difftime survive; execute/remove/rename/exit/getenv/tmpname
// are stripped). io is not opened at all. The lm.* API surface is bound in
// src/script/lua_bindings.cpp.

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <entt/entt.hpp>
#include <sol/sol.hpp>

namespace liminal {

class Scene;
class Window;
class Audio;
class AssetCache;
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
    std::unordered_map<std::string, ScriptFile> m_files;      // by Script.path
    std::unordered_map<entt::entity, Instance> m_instances;
    std::unordered_set<std::string> m_reportedErrors;
    std::function<void(const std::string&)> m_errorSink;
    float m_reloadTimer = 0.0f;
    std::chrono::steady_clock::time_point m_epoch;
};

} // namespace liminal
