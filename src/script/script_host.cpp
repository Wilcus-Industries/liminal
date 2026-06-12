// ScriptHost implementation. Design notes live in the header; the load path
// in brief: Script.path -> Assets::resolve -> file source cached per path ->
// per-entity sol::environment runs the chunk -> chunk returns the behavior
// table -> on_start once, on_update per frame, everything pcall'd.
#include <liminal/script/script_host.hpp>

#include "lua_bindings.hpp"

#include <liminal/core/assets.hpp>
#include <liminal/scene/component_registry.hpp>
#include <liminal/scene/scene.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>

namespace liminal {

namespace fs = std::filesystem;

ScriptHost::ScriptHost(Window* input)
    : m_input(input), m_epoch(std::chrono::steady_clock::now()) {
    m_lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                         sol::lib::table, sol::lib::os);
    // The os library is opened only for time/clock/date/difftime; strip the
    // process/filesystem-touching members. io is never opened.
    sol::table os = m_lua["os"];
    for (const char* k : {"execute", "remove", "rename", "exit", "getenv",
                          "tmpname", "setlocale"}) {
        os[k] = sol::lua_nil;
    }

    luabind::bindCore(m_lua, *this);

    // Component usertypes hang off the registry so app-registered components
    // can join in (registerComponent's luaBind parameter).
    registerBuiltinComponents();
    for (const auto& ops : ComponentRegistry::instance().all()) {
        if (ops.luaBind) ops.luaBind(&m_lua);
    }
}

ScriptHost::~ScriptHost() = default;

double ScriptHost::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         m_epoch)
        .count();
}

void ScriptHost::reportError(const std::string& path,
                             const std::string& message) {
    const std::string key = path + ": " + message;
    if (!m_reportedErrors.insert(key).second) return; // already reported
    std::fprintf(stderr, "[lua] %s\n", key.c_str());
    if (m_errorSink) m_errorSink(key);
}

ScriptHost::ScriptFile& ScriptHost::ensureFile(const std::string& path) {
    auto it = m_files.find(path);
    if (it != m_files.end()) return it->second;

    ScriptFile file;
    file.resolved = Assets::resolve(path);
    std::ifstream in(file.resolved, std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        file.source = ss.str();
        std::error_code ec;
        file.mtime = fs::last_write_time(file.resolved, ec);
        file.ok = true;
    } else {
        reportError(path, "cannot open script file (resolved to '" +
                              file.resolved + "')");
    }
    return m_files.emplace(path, std::move(file)).first->second;
}

void ScriptHost::initInstance(Instance& inst, entt::entity e, Scene& scene) {
    inst.started = false;
    inst.failed = false;
    inst.table = sol::table{};

    ScriptFile& file = ensureFile(inst.path);
    if (!file.ok) {
        inst.failed = true;
        return;
    }

    // Fresh environment per entity: globals readable, writes stay local, the
    // chunk's locals and returned table are unique to this entity.
    inst.env = sol::environment(m_lua, sol::create, m_lua.globals());

    sol::load_result chunk = m_lua.load(file.source, "@" + file.resolved);
    if (!chunk.valid()) {
        sol::error err = chunk;
        reportError(inst.path, err.what());
        inst.failed = true;
        return;
    }
    sol::protected_function fn = chunk;
    inst.env.set_on(fn);

    sol::protected_function_result result = fn();
    if (!result.valid()) {
        sol::error err = result;
        reportError(inst.path, err.what());
        inst.failed = true;
        return;
    }
    sol::object ret = result;
    if (ret.get_type() != sol::type::table) {
        reportError(inst.path, "script must return a table "
                               "{ on_start = ..., on_update = ... }");
        inst.failed = true;
        return;
    }
    inst.table = ret.as<sol::table>();

    sol::object onStart = inst.table["on_start"];
    if (onStart.is<sol::protected_function>()) {
        sol::protected_function_result r =
            onStart.as<sol::protected_function>()(Entity(e, &scene));
        if (!r.valid()) {
            sol::error err = r;
            reportError(inst.path, err.what());
            inst.failed = true;
            return;
        }
    }
    inst.started = true;
}

void ScriptHost::pollReloads() {
    std::vector<std::string> changed;
    for (auto& [path, file] : m_files) {
        std::error_code ec;
        if (!file.ok) {
            // A previously-missing file that appears counts as a change.
            if (fs::exists(Assets::resolve(path), ec)) changed.push_back(path);
            continue;
        }
        const auto mtime = fs::last_write_time(file.resolved, ec);
        if (!ec && mtime != file.mtime) changed.push_back(path);
    }

    for (const std::string& path : changed) {
        m_files.erase(path); // re-resolve + re-read lazily via ensureFile
        // Old errors may be fixed now; let them report again if not.
        m_reportedErrors.clear();
        std::printf("[lua] reloaded %s\n", path.c_str());
        std::fflush(stdout);
        for (auto& [entity, inst] : m_instances) {
            if (inst.path == path) { // re-init below
                inst.started = false;
                inst.failed = false;
            }
        }
    }
}

void ScriptHost::update(Scene& scene, float dt) {
    m_scene = &scene;

    m_reloadTimer += dt;
    if (m_reloadTimer >= 0.5f) {
        m_reloadTimer = 0.0f;
        pollReloads();
    }

    // Snapshot first: scripts may create/destroy entities mid-iteration.
    std::vector<std::pair<entt::entity, std::string>> active;
    scene.each<Script>([&](Entity e, Script& s) {
        active.emplace_back(e.handle(), s.path);
    });

    // Drop state for entities that vanished or lost their Script.
    for (auto it = m_instances.begin(); it != m_instances.end();) {
        const bool alive = std::any_of(
            active.begin(), active.end(),
            [&](const auto& p) { return p.first == it->first; });
        it = alive ? std::next(it) : m_instances.erase(it);
    }

    for (auto& [entity, path] : active) {
        auto it = m_instances.find(entity);
        if (it == m_instances.end() || it->second.path != path) {
            Instance inst;
            inst.path = path;
            it = m_instances.insert_or_assign(entity, std::move(inst)).first;
        }
        Instance& inst = it->second;
        if (!inst.started && !inst.failed) initInstance(inst, entity, scene);
        if (inst.failed || !inst.started) continue;

        sol::object onUpdate = inst.table["on_update"];
        if (!onUpdate.is<sol::protected_function>()) continue;
        Entity self(entity, &scene);
        if (!self.valid()) continue; // destroyed earlier this frame
        sol::protected_function_result r =
            onUpdate.as<sol::protected_function>()(self, dt);
        if (!r.valid()) {
            sol::error err = r;
            reportError(inst.path, err.what());
            // Keep running next frame; dedupe stops the spam. Persistent
            // errors are visible once, fixed files re-report after reload.
        }
    }

    m_scene = nullptr;
}

} // namespace liminal
