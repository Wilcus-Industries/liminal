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

#ifndef LIMINAL_WITH_SCRIPTING
#error "liminal/script/script_host.hpp requires the scripting module; configure liminal with LIMINAL_WITH_SCRIPTING=ON"
#endif

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

class ScriptHost {
public:
    // `input` powers lm.input.key_down; pass nullptr for a windowless host
    // (input queries then return false). App wires its Window in.
    explicit ScriptHost(Window* input = nullptr);
    ~ScriptHost();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Drives every Script component in `scene`. Call once per frame.
    void update(Scene& scene, float dt);

    // Escape hatch: the raw state, for apps that want to bind extra API.
    sol::state& lua() { return m_lua; }

    // Valid during update() (lm.scene closes over the host); null outside.
    Scene* activeScene() { return m_scene; }
    Window* input() { return m_input; }
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
        std::filesystem::file_time_type mtime{}; // for hot reload
        bool ok = false;                         // file read successfully
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
    Window* m_input = nullptr;
    Scene* m_scene = nullptr;
    std::unordered_map<std::string, ScriptFile> m_files;      // by Script.path
    std::unordered_map<entt::entity, Instance> m_instances;
    std::unordered_set<std::string> m_reportedErrors;
    std::function<void(const std::string&)> m_errorSink;
    float m_reloadTimer = 0.0f;
    std::chrono::steady_clock::time_point m_epoch;
};

} // namespace liminal
