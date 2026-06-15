// The lm.* Lua API. Binding choices, for the record:
//
// - Transform/MeshRenderer fields are bound as plain member variables. sol2
//   returns registered usertypes BY REFERENCE from member-variable access,
//   so `t.position.y = 2` mutates the real component (spin.lua asserts this
//   at on_start as a regression tripwire). Explicit set_position/set_rotation
//   /set_scale/set_color helpers exist anyway — they read better for the
//   common "set all three" case.
// - Entity is the cheap {entt::entity, Scene*} value handle, copied into Lua
//   userdata. get_transform() auto-adds a Transform so scripts never crash on
//   a bare entity; get_mesh_renderer() returns nil when absent (renderers are
//   never implicitly added).
// - entity:get_component("X") goes through ComponentRegistry::getRaw plus a
//   name->pusher map that each component binder fills in. Pointers returned
//   by getRaw are entt component pointers: stable for the frame, not across
//   structural changes — scripts should not cache components across frames.
#include "lua_bindings.hpp"

#include <liminal/audio/audio.hpp>
#include <liminal/core/asset_cache.hpp>
#include <liminal/core/assets.hpp>
#include <liminal/core/window.hpp>
#if defined(LIMINAL_WITH_INFERENCE)
#include <liminal/inference/engine.hpp>
#endif
#include <liminal/render/mesh.hpp>
#include <liminal/scene/component_registry.hpp>
#include <liminal/scene/scene.hpp>
#include <liminal/script/script_host.hpp>

#include <cctype>
#include <cstdio>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace liminal::luabind {

namespace {

using Pusher = sol::object (*)(lua_State*, void*);

std::unordered_map<std::string, Pusher>& pushers() {
    static std::unordered_map<std::string, Pusher> map;
    return map;
}

} // namespace

sol::object pushComponent(lua_State* L, const std::string& name, void* raw) {
    auto it = pushers().find(name);
    if (it == pushers().end() || !raw) return sol::make_object(L, sol::lua_nil);
    return it->second(L, raw);
}

// --- component binders (ComponentOps::luaBind targets) ----------------------

void bindName(void* luaState) {
    auto& lua = *static_cast<sol::state*>(luaState);
    lua.new_usertype<Name>("Name", sol::no_constructor,
                           "value", &Name::value);
    pushers()["Name"] = [](lua_State* L, void* raw) {
        return sol::make_object(L, static_cast<Name*>(raw));
    };
}

void bindTransform(void* luaState) {
    auto& lua = *static_cast<sol::state*>(luaState);
    lua.new_usertype<Transform>(
        "Transform", sol::no_constructor,
        "position", &Transform::position,
        "rotation", &Transform::rotationEuler, // degrees, yaw->pitch->roll
        "scale", &Transform::scale,
        "set_position",
        [](Transform& t, float x, float y, float z) { t.position = {x, y, z}; },
        "set_rotation",
        [](Transform& t, float x, float y, float z) { t.rotationEuler = {x, y, z}; },
        "set_scale",
        [](Transform& t, float x, float y, float z) { t.scale = {x, y, z}; });
    pushers()["Transform"] = [](lua_State* L, void* raw) {
        return sol::make_object(L, static_cast<Transform*>(raw));
    };
}

void bindMeshRenderer(void* luaState) {
    auto& lua = *static_cast<sol::state*>(luaState);
    lua.new_usertype<MeshRenderer>(
        "MeshRenderer", sol::no_constructor,
        "mesh", &MeshRenderer::meshAsset,
        "texture", &MeshRenderer::textureAsset,
        "color", &MeshRenderer::color,
        "set_color",
        [](MeshRenderer& mr, float r, float g, float b, sol::optional<float> a) {
            mr.color = {r, g, b, a.value_or(1.0f)};
        });
    pushers()["MeshRenderer"] = [](lua_State* L, void* raw) {
        return sol::make_object(L, static_cast<MeshRenderer*>(raw));
    };
}

void bindCamera(void* luaState) {
    auto& lua = *static_cast<sol::state*>(luaState);
    lua.new_usertype<Camera>(
        "Camera", sol::no_constructor,
        "fov", &Camera::fovDeg,
        "near", &Camera::nearZ,
        "far", &Camera::farZ,
        "primary", &Camera::primary,
        "shader", &Camera::shaderName);
    pushers()["Camera"] = [](lua_State* L, void* raw) {
        return sol::make_object(L, static_cast<Camera*>(raw));
    };
}

// --- core API ----------------------------------------------------------------

namespace {

void bindVecTypes(sol::state& lua) {
    lua.new_usertype<glm::vec3>(
        "vec3",
        sol::call_constructor,
        sol::constructors<glm::vec3(), glm::vec3(float),
                          glm::vec3(float, float, float)>(),
        "x", &glm::vec3::x, "y", &glm::vec3::y, "z", &glm::vec3::z,
        sol::meta_function::addition,
        [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
        sol::meta_function::subtraction,
        [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
        sol::meta_function::multiplication,
        sol::overload([](const glm::vec3& a, float s) { return a * s; },
                      [](float s, const glm::vec3& a) { return a * s; }),
        sol::meta_function::unary_minus,
        [](const glm::vec3& a) { return -a; },
        sol::meta_function::to_string,
        [](const glm::vec3& v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "vec3(%g, %g, %g)", v.x, v.y, v.z);
            return std::string(buf);
        },
        "length", [](const glm::vec3& v) { return glm::length(v); });

    lua.new_usertype<glm::vec4>(
        "vec4",
        sol::call_constructor,
        sol::constructors<glm::vec4(), glm::vec4(float),
                          glm::vec4(float, float, float, float)>(),
        "x", &glm::vec4::x, "y", &glm::vec4::y,
        "z", &glm::vec4::z, "w", &glm::vec4::w,
        sol::meta_function::to_string,
        [](const glm::vec4& v) {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "vec4(%g, %g, %g, %g)", v.x, v.y,
                          v.z, v.w);
            return std::string(buf);
        });
}

void bindEntity(sol::state& lua) {
    lua.new_usertype<Entity>(
        "Entity", sol::no_constructor,
        "name",
        sol::property(
            [](Entity& e) -> std::string {
                return (e.valid() && e.has<Name>()) ? e.get<Name>().value : "";
            },
            [](Entity& e, const std::string& n) {
                if (e.valid()) e.add<Name>(Name{n});
            }),
        "valid", [](Entity& e) { return e.valid(); },
        "destroy", [](Entity& e) { e.destroy(); },
        "get_transform",
        [](Entity& e, sol::this_state s) -> sol::object {
            if (!e.valid()) return sol::make_object(s, sol::lua_nil);
            if (!e.has<Transform>()) e.add<Transform>(Transform{});
            return sol::make_object(s, &e.get<Transform>());
        },
        "get_mesh_renderer",
        [](Entity& e, sol::this_state s) -> sol::object {
            if (!e.valid() || !e.has<MeshRenderer>())
                return sol::make_object(s, sol::lua_nil);
            return sol::make_object(s, &e.get<MeshRenderer>());
        },
        "get_component",
        [](Entity& e, const std::string& name, sol::this_state s) -> sol::object {
            if (!e.valid() || !e.scene()) return sol::make_object(s, sol::lua_nil);
            const ComponentOps* ops = ComponentRegistry::instance().find(name);
            if (!ops || !ops->getRaw) return sol::make_object(s, sol::lua_nil);
            return pushComponent(s, name,
                                 ops->getRaw(e.scene()->registry(), e.handle()));
        });
}

// Warn-once-per-session sink shared by the null-subsystem guards. Each distinct
// message prints through the same channel as lm.log exactly once.
void warnOnce(const std::string& message) {
    static std::unordered_set<std::string> seen;
    if (!seen.insert(message).second) return;
    std::printf("[lua] %s\n", message.c_str());
    std::fflush(stdout);
}

// --- lm.audio ----------------------------------------------------------------
// name -> AudioParams member maps. Game thread touches only atomics, which is
// exactly what these do. Unknown names warn once and otherwise no-op.

using FloatField = std::atomic<float> AudioParams::*;
using IntField = std::atomic<int> AudioParams::*;
using CounterField = std::atomic<uint32_t> AudioParams::*;

const std::unordered_map<std::string, FloatField>& audioFloats() {
    static const std::unordered_map<std::string, FloatField> m = {
        {"gain", &AudioParams::gain},
        {"decay", &AudioParams::decay},
        {"dread", &AudioParams::dread},
        {"breath", &AudioParams::breathGain},
        {"lamp_hum", &AudioParams::lampHumGain},
        {"zone_blend", &AudioParams::zoneBlend},
        {"ambient_gain", &AudioParams::ambientGain},
        {"water_gain", &AudioParams::waterGain},
        {"throb_level", &AudioParams::throbLevel},
        {"breath_level", &AudioParams::breathLevel},
        {"step_level", &AudioParams::stepLevel},
        {"water_level", &AudioParams::waterLevel},
        {"dropout_min", &AudioParams::dropoutMin},
        {"dropout_max", &AudioParams::dropoutMax},
        {"interval_min", &AudioParams::intervalMin},
        {"interval_max", &AudioParams::intervalMax},
    };
    return m;
}

const std::unordered_map<std::string, IntField>& audioInts() {
    static const std::unordered_map<std::string, IntField> m = {
        {"zone_a", &AudioParams::zoneTagA},
        {"zone_b", &AudioParams::zoneTagB},
        {"water_tag", &AudioParams::waterTag},
    };
    return m;
}

const std::unordered_map<std::string, CounterField>& audioEvents() {
    static const std::unordered_map<std::string, CounterField> m = {
        {"step", &AudioParams::stepCounter},
        {"jump", &AudioParams::jumpCounter},
        {"mumble", &AudioParams::mumbleCounter},
        {"door_creak", &AudioParams::doorCreakCounter},
    };
    return m;
}

void bindAudio(sol::table& lm, sol::state& lua, ScriptHost& host) {
    sol::table audio = lua.create_table();

    audio["set"] = [&host](const std::string& name, sol::object value) {
        Audio* a = host.context().audio;
        if (!a) {
            warnOnce("lm.audio.set: no audio subsystem (no-op)");
            return;
        }
        AudioParams& p = a->params;
        if (auto it = audioFloats().find(name); it != audioFloats().end()) {
            (p.*(it->second)).store(value.is<double>()
                                        ? float(value.as<double>())
                                        : 0.0f);
            return;
        }
        if (auto it = audioInts().find(name); it != audioInts().end()) {
            // Accept the double subtype too: a Lua literal like 2.0 reports
            // is<int>()==false and would otherwise silently store 0. Matches
            // the dual-subtype convention used by the seed read and procgen.
            (p.*(it->second))
                .store(value.is<int>() ? value.as<int>()
                       : value.is<double>() ? int(value.as<double>())
                                            : 0);
            return;
        }
        if (name == "enabled") {
            p.enabled.store(value.is<bool>() ? value.as<bool>() : false);
            return;
        }
        warnOnce("lm.audio.set: unknown name '" + name + "'");
    };

    audio["get"] = [&host](const std::string& name,
                           sol::this_state s) -> sol::object {
        Audio* a = host.context().audio;
        if (!a) {
            warnOnce("lm.audio.get: no audio subsystem (returns 0)");
            // Match the with-subsystem path's Lua type: int-typed names return
            // an integer 0, everything else a double 0.0.
            if (audioInts().count(name)) return sol::make_object(s, 0);
            return sol::make_object(s, 0.0);
        }
        AudioParams& p = a->params;
        if (auto it = audioFloats().find(name); it != audioFloats().end())
            return sol::make_object(s, double((p.*(it->second)).load()));
        if (auto it = audioInts().find(name); it != audioInts().end())
            return sol::make_object(s, (p.*(it->second)).load());
        if (name == "enabled")
            return sol::make_object(s, p.enabled.load());
        warnOnce("lm.audio.get: unknown name '" + name + "'");
        return sol::make_object(s, 0.0);
    };

    audio["event"] = [&host](const std::string& name) {
        Audio* a = host.context().audio;
        if (!a) {
            warnOnce("lm.audio.event: no audio subsystem (no-op)");
            return;
        }
        if (auto it = audioEvents().find(name); it != audioEvents().end()) {
            (a->params.*(it->second)).fetch_add(1);
            return;
        }
        warnOnce("lm.audio.event: unknown event '" + name + "'");
    };

    audio["ok"] = [&host]() {
        Audio* a = host.context().audio;
        return a ? a->ok() : false;
    };

    lm["audio"] = audio;
}

// --- lm.assets ---------------------------------------------------------------

void bindAssets(sol::table& lm, sol::state& lua, ScriptHost& host) {
    // MeshData is bound opaque here (no constructor): chunk 4 will add the
    // factories that produce one. Scripts only pass it through to add_mesh.
    lua.new_usertype<MeshData>("MeshData", sol::no_constructor);

    sol::table assets = lua.create_table();
    assets["add_mesh"] = [&host](const std::string& name,
                                 const MeshData& data) -> std::string {
        AssetCache* ac = host.context().assets;
        if (!ac) {
            warnOnce("lm.assets.add_mesh: no asset cache (no-op)");
            return name;
        }
        return ac->addMesh(name, data); // returns the resolvable storage key
    };
    lm["assets"] = assets;
}

// --- lm.ai -------------------------------------------------------------------
// Local LLM inference. The table exists whenever liminal is built with
// inference; the underlying engine pointer may still be null (e.g. tests, or a
// host that opted out), in which case every call warns once and returns a
// sensible default. Scripts feature-test with `if lm.ai then`.
//
// Lifecycle: lm.ai.start{model=...} spawns the worker + loads the model;
// lm.ai.submit{...} -> id; poll(id) until complete/failed; then call
// lm.ai.forget(id) — finished responses are retained until forgotten, so a
// script that never forgets will leak responses for the engine's lifetime.
#if defined(LIMINAL_WITH_INFERENCE)

// Read an integer field from a Lua table only when present (else keep `out`).
void optInt(const sol::table& t, const char* key, int& out) {
    sol::object o = t[key];
    if (o.is<int>()) out = o.as<int>();
    else if (o.is<double>()) out = int(o.as<double>());
}
void optFloat(const sol::table& t, const char* key, float& out) {
    sol::object o = t[key];
    if (o.is<double>()) out = float(o.as<double>());
}
void optString(const sol::table& t, const char* key, std::string& out) {
    sol::object o = t[key];
    if (o.is<std::string>()) out = o.as<std::string>();
}

void bindAi(sol::table& lm, sol::state& lua, ScriptHost& host) {
    sol::table ai = lua.create_table();

    ai["start"] = [&host](sol::table opts) {
        inference::Engine* eng = host.context().inference;
        if (!eng) {
            warnOnce("lm.ai.start: no inference engine (no-op)");
            return;
        }
        sol::object model = opts["model"];
        if (!model.is<std::string>()) {
            warnOnce("lm.ai.start: 'model' (string) is required (no-op)");
            return;
        }
        inference::EngineConfig cfg;
        cfg.modelPath = Assets::resolve(model.as<std::string>());
        optInt(opts, "context", cfg.contextTokens);
        optInt(opts, "threads", cfg.threads);
        optInt(opts, "gpu_layers", cfg.gpuLayers);
        eng->start(cfg);
    };

    ai["stop"] = [&host]() {
        inference::Engine* eng = host.context().inference;
        if (!eng) {
            warnOnce("lm.ai.stop: no inference engine (no-op)");
            return;
        }
        eng->stop();
    };

    ai["status"] = [&host](sol::this_state s)
        -> std::tuple<std::string, std::string> {
        inference::Engine* eng = host.context().inference;
        (void)s;
        if (!eng) {
            warnOnce("lm.ai.status: no inference engine (returns 'stopped')");
            return {"stopped", ""};
        }
        const char* name = "stopped";
        switch (eng->status()) {
            case inference::Engine::Status::Stopped: name = "stopped"; break;
            case inference::Engine::Status::Loading: name = "loading"; break;
            case inference::Engine::Status::Ready:   name = "ready";   break;
            case inference::Engine::Status::Failed:  name = "failed";  break;
        }
        return {name, eng->statusMessage()};
    };

    ai["submit"] = [&host](sol::table opts) -> lua_Integer {
        inference::Engine* eng = host.context().inference;
        if (!eng) {
            warnOnce("lm.ai.submit: no inference engine (returns 0)");
            return 0;
        }
        inference::PromptRequest req;
        optString(opts, "system", req.system);
        optString(opts, "user", req.user);
        optString(opts, "grammar", req.grammarGbnf);
        optFloat(opts, "temperature", req.sampling.temperature);
        optFloat(opts, "top_p", req.sampling.topP);
        optInt(opts, "top_k", req.sampling.topK);
        optFloat(opts, "repeat_penalty", req.sampling.repeatPenalty);
        optInt(opts, "max_tokens", req.sampling.maxTokens);
        // seed is uint32; accept it whether Lua reports the value as an integer
        // (Lua 5.4 integer subtype, e.g. seed=42) or a double. Mirrors optInt's
        // dual check — is<double>() alone drops integer-subtype values.
        {
            sol::object o = opts["seed"];
            if (o.is<int64_t>())
                req.sampling.seed = uint32_t(o.as<int64_t>());
            else if (o.is<double>())
                req.sampling.seed = uint32_t(int64_t(o.as<double>()));
        }
        return lua_Integer(eng->submit(std::move(req)));
    };

    ai["poll"] = [&host](lua_Integer id, sol::this_state s) -> sol::table {
        sol::state_view sv(s);
        sol::table out = sv.create_table();
        inference::Engine* eng = host.context().inference;
        if (!eng) {
            warnOnce("lm.ai.poll: no inference engine (returns empty response)");
            out["text"] = "";
            out["complete"] = false;
            out["failed"] = false;
            out["error"] = "";
            return out;
        }
        inference::Response r = eng->poll(inference::RequestId(id));
        out["text"] = r.text;
        out["complete"] = r.complete;
        out["failed"] = r.failed;
        out["error"] = r.error;
        return out;
    };

    ai["cancel"] = [&host](lua_Integer id) {
        inference::Engine* eng = host.context().inference;
        if (!eng) {
            warnOnce("lm.ai.cancel: no inference engine (no-op)");
            return;
        }
        eng->cancel(inference::RequestId(id));
    };

    ai["forget"] = [&host](lua_Integer id) {
        inference::Engine* eng = host.context().inference;
        if (!eng) {
            warnOnce("lm.ai.forget: no inference engine (no-op)");
            return;
        }
        eng->forget(inference::RequestId(id));
    };

    ai["busy"] = [&host]() -> bool {
        inference::Engine* eng = host.context().inference;
        if (!eng) return false;
        return eng->busy();
    };

    ai["queue_depth"] = [&host]() -> lua_Integer {
        inference::Engine* eng = host.context().inference;
        if (!eng) return 0;
        return lua_Integer(eng->queueDepth());
    };

    lm["ai"] = ai;
}
#endif // LIMINAL_WITH_INFERENCE

} // namespace

void bindCore(sol::state& lua, ScriptHost& host) {
    bindVecTypes(lua);
    bindEntity(lua);

    sol::table lm = lua.create_named_table("lm");
    lm["log"] = [&host](const std::string& msg) { host.emitLog(msg); };
    lm["vec3"] = lua["vec3"];
    lm["vec4"] = lua["vec4"];

    sol::table scene = lua.create_table();
    scene["find"] = [&host](const std::string& name,
                            sol::this_state s) -> sol::object {
        Scene* sc = host.activeScene();
        if (!sc) return sol::make_object(s, sol::lua_nil);
        Entity e = sc->find(name);
        if (!e.valid()) return sol::make_object(s, sol::lua_nil);
        return sol::make_object(s, e);
    };
    scene["create"] = [&host](const std::string& name) -> Entity {
        Scene* sc = host.activeScene();
        return sc ? sc->create(name) : Entity{};
    };
    // All entities whose Name matches, as an array (empty when none / no scene).
    scene["find_all"] = [&host](sol::this_state s,
                                const std::string& name) -> sol::table {
        sol::state_view sv(s);
        sol::table out = sv.create_table();
        Scene* sc = host.activeScene();
        if (!sc) return out;
        int i = 1;
        sc->each<Name>([&](Entity e, Name& n) {
            if (n.value == name) out[i++] = e;
        });
        return out;
    };
    // Snapshot the entity list, then call fn(entity) for each — safe against
    // structural changes the callback makes (create/destroy mid-iteration).
    scene["each"] = [&host](sol::protected_function fn) {
        Scene* sc = host.activeScene();
        if (!sc || !fn.valid()) return;
        // Snapshot every live entity before calling out, so a callback that
        // creates/destroys entities can't invalidate the iteration.
        std::vector<entt::entity> snapshot;
        for (auto h : sc->registry().view<entt::entity>())
            snapshot.push_back(h);
        for (entt::entity h : snapshot) {
            Entity e(h, sc);
            if (!e.valid()) continue; // destroyed by an earlier callback
            sol::protected_function_result r = fn(e);
            (void)r; // errors surface through the host's pcall reporting
        }
    };
    scene["destroy"] = [](sol::object e) {
        if (e.is<Entity>()) {
            Entity ent = e.as<Entity>();
            if (ent.valid()) ent.destroy();
        }
    };
    scene["change"] = [&host](const std::string& path) {
        const auto& fn = host.context().requestSceneChange;
        if (fn) fn(path);
        else warnOnce("lm.scene.change: no scene-change handler (ignored)");
    };
    lm["scene"] = scene;

    sol::table input = lua.create_table();
    // Accepts a GLFW keycode (number) or a single-character string ("w",
    // "A") — letter keycodes are their uppercase ASCII values. Windowless
    // hosts always report false.
    input["key_down"] = [&host](sol::object key) -> bool {
        Window* w = host.input();
        if (!w) return false;
        if (key.is<int>()) return w->keyDown(key.as<int>());
        if (key.is<std::string>()) {
            const std::string s = key.as<std::string>();
            if (s.size() == 1)
                return w->keyDown(std::toupper(static_cast<unsigned char>(s[0])));
        }
        return false;
    };
    // Mouse-look: accumulated cursor delta since the last call (zeroed on read),
    // cursor capture toggle (hides+locks the cursor for relative look), and raw
    // mouse-button state (GLFW button index, 0 = left).
    input["mouse_delta"] = [&host]() -> std::tuple<float, float> {
        Window* w = host.input();
        if (!w) return {0.0f, 0.0f};
        float dx = 0.0f, dy = 0.0f;
        w->mouseDelta(dx, dy);
        return {dx, dy};
    };
    input["set_cursor_captured"] = [&host](bool captured) {
        Window* w = host.input();
        if (w) w->setCursorCaptured(captured);
    };
    input["cursor_captured"] = [&host]() -> bool {
        Window* w = host.input();
        return w ? w->cursorCaptured() : false;
    };
    input["mouse_down"] = [&host](int button) -> bool {
        Window* w = host.input();
        return w ? w->mouseDown(button) : false;
    };
    lm["input"] = input;

    sol::table time = lua.create_table();
    time["now"] = [&host]() { return host.now(); };
    lm["time"] = time;

    bindAudio(lm, lua, host);
    bindAssets(lm, lua, host);
    bindProcgen(lm, lua);
#if defined(LIMINAL_WITH_INFERENCE)
    bindAi(lm, lua, host);
#endif
}

} // namespace liminal::luabind
