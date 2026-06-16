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
#include <liminal/render/renderer.hpp>
#include <liminal/render/texture.hpp>
#include <liminal/scene/component_registry.hpp>
#include <liminal/scene/components.hpp>
#include <liminal/scene/raycast.hpp>
#include <liminal/scene/scene.hpp>
#include <liminal/script/script_host.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <optional>
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

// --- Lua <-> JSON converters ------------------------------------------------
// File-scope so later bindings (e.g. entity:add_component) can reuse them.
// A Lua table with contiguous integer keys 1..n maps to a JSON array; any
// other table maps to a JSON object. Functions/userdata encode as null.
nlohmann::json luaToJson(const sol::object& obj) {
    switch (obj.get_type()) {
        case sol::type::lua_nil:
        case sol::type::none:
            return nullptr;
        case sol::type::boolean:
            return obj.as<bool>();
        case sol::type::number: {
            double d = obj.as<double>();
            lua_Integer i = obj.as<lua_Integer>();
            if (static_cast<double>(i) == d)
                return nlohmann::json(static_cast<std::int64_t>(i));
            return d;
        }
        case sol::type::string:
            return obj.as<std::string>();
        case sol::type::table: {
            sol::table t = obj.as<sol::table>();
            // Detect a contiguous 1..n integer-keyed array.
            std::size_t n = t.size();
            bool isArray = n > 0;
            if (isArray) {
                for (std::size_t k = 1; k <= n; ++k) {
                    if (!t[k].valid()) { isArray = false; break; }
                }
            }
            if (isArray) {
                nlohmann::json arr = nlohmann::json::array();
                for (std::size_t k = 1; k <= n; ++k)
                    arr.push_back(luaToJson(t[k]));
                return arr;
            }
            nlohmann::json o = nlohmann::json::object();
            for (auto& kv : t) {
                if (kv.first.get_type() == sol::type::string)
                    o[kv.first.as<std::string>()] = luaToJson(kv.second);
                else if (kv.first.get_type() == sol::type::number)
                    o[std::to_string(kv.first.as<lua_Integer>())] =
                        luaToJson(kv.second);
            }
            return o;
        }
        default:
            return nullptr;
    }
}

sol::object jsonToLua(sol::state_view lua, const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::null:
            return sol::make_object(lua, sol::lua_nil);
        case nlohmann::json::value_t::boolean:
            return sol::make_object(lua, j.get<bool>());
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
            return sol::make_object(lua, j.get<lua_Integer>());
        case nlohmann::json::value_t::number_float:
            return sol::make_object(lua, j.get<double>());
        case nlohmann::json::value_t::string:
            return sol::make_object(lua, j.get<std::string>());
        case nlohmann::json::value_t::array: {
            sol::table t = lua.create_table();
            int i = 1;
            for (const auto& e : j) t[i++] = jsonToLua(lua, e);
            return t;
        }
        case nlohmann::json::value_t::object: {
            sol::table t = lua.create_table();
            for (auto it = j.begin(); it != j.end(); ++it)
                t[it.key()] = jsonToLua(lua, it.value());
            return t;
        }
        default:
            return sol::make_object(lua, sol::lua_nil);
    }
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
        "color2", &MeshRenderer::color2,
        // set_color also matches color2 so scripts that only call set_color stay
        // uniform; call set_color2 explicitly to make a vertical gradient.
        "set_color",
        [](MeshRenderer& mr, float r, float g, float b, sol::optional<float> a) {
            mr.color = {r, g, b, a.value_or(1.0f)};
            mr.color2 = mr.color;
        },
        "set_color2",
        [](MeshRenderer& mr, float r, float g, float b, sol::optional<float> a) {
            mr.color2 = {r, g, b, a.value_or(1.0f)};
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

void bindCollider(void* luaState) {
    auto& lua = *static_cast<sol::state*>(luaState);
    lua.new_usertype<Collider>(
        "Collider", sol::no_constructor,
        "center", &Collider::center,
        "half_extents", &Collider::halfExtents);
    pushers()["Collider"] = [](lua_State* L, void* raw) {
        return sol::make_object(L, static_cast<Collider*>(raw));
    };
}

void bindBillboard(void* luaState) {
    auto& lua = *static_cast<sol::state*>(luaState);
    lua.new_usertype<Billboard>(
        "Billboard", sol::no_constructor,
        "yaw_only", &Billboard::yawOnly);
    pushers()["Billboard"] = [](lua_State* L, void* raw) {
        return sol::make_object(L, static_cast<Billboard*>(raw));
    };
}

void bindLight(void* luaState) {
    auto& lua = *static_cast<sol::state*>(luaState);
    lua.new_usertype<Light>(
        "Light", sol::no_constructor,
        "color", &Light::color,
        "intensity", &Light::intensity,
        "set_color",
        [](Light& l, float r, float g, float b) { l.color = {r, g, b}; });
    pushers()["Light"] = [](lua_State* L, void* raw) {
        return sol::make_object(L, static_cast<Light*>(raw));
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
        },
        "add_component",
        [](Entity& e, const std::string& name, sol::optional<sol::table> data,
           sol::this_state s) -> sol::object {
            if (!e.valid() || !e.scene()) return sol::make_object(s, sol::lua_nil);
            const ComponentOps* ops = ComponentRegistry::instance().find(name);
            if (!ops || !ops->fromJson) return sol::make_object(s, sol::lua_nil);
            nlohmann::json j = data ? luaToJson(*data)
                                    : nlohmann::json::object();
            ops->fromJson(e.scene()->registry(), e.handle(), j);
            return pushComponent(
                s, name,
                ops->getRaw ? ops->getRaw(e.scene()->registry(), e.handle())
                            : nullptr);
        },
        "remove_component",
        [](Entity& e, const std::string& name) {
            if (!e.valid() || !e.scene()) return;
            const ComponentOps* ops = ComponentRegistry::instance().find(name);
            if (ops && ops->removeFrom)
                ops->removeFrom(e.scene()->registry(), e.handle());
        },
        "has_component",
        [](Entity& e, const std::string& name) -> bool {
            if (!e.valid() || !e.scene()) return false;
            const ComponentOps* ops = ComponentRegistry::instance().find(name);
            return ops && ops->has &&
                   ops->has(e.scene()->registry(), e.handle());
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
    assets["add_texture"] = sol::overload(
        // (name, path) — load an image file (PNG/JPEG/TGA/BMP) through the VFS.
        [&host](const std::string& name, const std::string& path) -> std::string {
            AssetCache* ac = host.context().assets;
            if (!ac) {
                warnOnce("lm.assets.add_texture: no asset cache (no-op)");
                return name;
            }
            std::optional<std::string> bytes = Assets::readFile(path);
            if (!bytes) {
                warnOnce("lm.assets.add_texture: cannot read image file");
                return name;
            }
            std::optional<Texture> tex = Texture::fromMemory(
                reinterpret_cast<const unsigned char*>(bytes->data()),
                bytes->size());
            if (!tex) {
                warnOnce("lm.assets.add_texture: cannot decode image");
                return name;
            }
            return ac->addTexture(name, std::move(*tex));
        },
        // (name, w, h, pixels) — RGBA8 from a flat Lua array (1-based, length
        // w*h*4, values 0..255). Backs procedural/pixel-art textures from Lua.
        [&host](const std::string& name, int w, int h, sol::table pixels)
            -> std::string {
            AssetCache* ac = host.context().assets;
            if (!ac) {
                warnOnce("lm.assets.add_texture: no asset cache (no-op)");
                return name;
            }
            const size_t need = static_cast<size_t>(w) * h * 4;
            if (w <= 0 || h <= 0 || pixels.size() < need) {
                warnOnce("lm.assets.add_texture: pixel table too small "
                         "(need w*h*4 bytes)");
                return name;
            }
            std::vector<unsigned char> rgba(need);
            for (size_t i = 0; i < need; ++i) {
                const int v = pixels.get_or(i + 1, 0); // 1-based Lua array
                rgba[i] = static_cast<unsigned char>(
                    v < 0 ? 0 : (v > 255 ? 255 : v));
            }
            std::optional<Texture> tex = Texture::fromPixels(w, h, rgba.data());
            if (!tex) {
                warnOnce("lm.assets.add_texture: upload failed");
                return name;
            }
            return ac->addTexture(name, std::move(*tex));
        });
    lm["assets"] = assets;
}

// --- lm.render ---------------------------------------------------------------
// Lua-settable render uniforms (G3): scripts drive fog/decay/lights and
// arbitrary named uniforms on the active scene shader, so custom shader packs
// can react per-area or to a decay clock. All resolve through
// host.context().renderer; no-ops (warn-once) in a windowless/renderer-less
// host.
void bindRender(sol::table& lm, sol::state& lua, ScriptHost& host) {
    sol::table render = lua.create_table();

    render["set_fog"] = [&host](float r, float g, float b, float density) {
        Renderer* rn = host.context().renderer;
        if (!rn) { warnOnce("lm.render.set_fog: no renderer (no-op)"); return; }
        rn->setFogColor(glm::vec3(r, g, b));
        rn->setFogDensity(density);
    };
    render["set_decay"] = [&host](float p) {
        Renderer* rn = host.context().renderer;
        if (!rn) { warnOnce("lm.render.set_decay: no renderer (no-op)"); return; }
        rn->setDecay(p);
    };
    render["set_light"] = [&host](float x, float y, float z) {
        Renderer* rn = host.context().renderer;
        if (!rn) { warnOnce("lm.render.set_light: no renderer (no-op)"); return; }
        rn->setLightDir(glm::vec3(x, y, z));
    };
    render["set_shade"] = [&host](float x, float y, float z) {
        Renderer* rn = host.context().renderer;
        if (!rn) { warnOnce("lm.render.set_shade: no renderer (no-op)"); return; }
        rn->setShadeDir(glm::vec3(x, y, z));
    };
    // set_uniform(name, number)  OR  set_uniform(name, vec3)
    render["set_uniform"] = sol::overload(
        [&host](const std::string& name, float v) {
            Renderer* rn = host.context().renderer;
            if (!rn) { warnOnce("lm.render.set_uniform: no renderer (no-op)"); return; }
            rn->setUniform(name, v);
        },
        [&host](const std::string& name, const glm::vec3& v) {
            Renderer* rn = host.context().renderer;
            if (!rn) { warnOnce("lm.render.set_uniform: no renderer (no-op)"); return; }
            rn->setUniform(name, v);
        });

    lm["render"] = render;
}

// --- lm.ui -------------------------------------------------------------------
// Immediate-mode screen-space 2D drawn into the scene render FBO each frame
// (origin top-left, render-target pixels). Resolves through
// host.context().renderer; no-ops (warn-once) without a renderer.
void bindUi(sol::table& lm, sol::state& lua, ScriptHost& host) {
    sol::table ui = lua.create_table();
    ui["text"] = [&host](float x, float y, const std::string& s,
                         float r, float g, float b,
                         sol::optional<float> a, sol::optional<float> scale) {
        Renderer* rn = host.context().renderer;
        if (!rn) { warnOnce("lm.ui.text: no renderer (no-op)"); return; }
        rn->uiText(x, y, s, glm::vec4(r, g, b, a.value_or(1.0f)),
                   scale.value_or(1.0f));
    };
    ui["rect"] = [&host](float x, float y, float w, float h,
                         float r, float g, float b, sol::optional<float> a) {
        Renderer* rn = host.context().renderer;
        if (!rn) { warnOnce("lm.ui.rect: no renderer (no-op)"); return; }
        rn->uiRect(x, y, w, h, glm::vec4(r, g, b, a.value_or(1.0f)));
    };
    ui["quad"] = ui["rect"]; // alias
    ui["line"] = [&host](float x0, float y0, float x1, float y1,
                         float r, float g, float b,
                         sol::optional<float> a, sol::optional<float> thick) {
        Renderer* rn = host.context().renderer;
        if (!rn) { warnOnce("lm.ui.line: no renderer (no-op)"); return; }
        rn->uiLine(x0, y0, x1, y1, glm::vec4(r, g, b, a.value_or(1.0f)),
                   thick.value_or(1.0f));
    };
    ui["size"] = [&host]() -> std::tuple<int, int> {
        Renderer* rn = host.context().renderer;
        if (!rn) { warnOnce("lm.ui.size: no renderer (0,0)"); return {0, 0}; }
        int w = 0, h = 0; rn->uiSize(w, h); return {w, h};
    };
    lm["ui"] = ui;
}

// --- lm.physics --------------------------------------------------------------
// Ray-vs-scene and AABB-overlap queries over the active scene's Colliders /
// mesh bounds. Both resolve through host.activeScene() + the asset cache, so
// they're no-ops (nil / false) in a windowless or scene-less host.
void bindPhysics(sol::table& lm, sol::state& lua, ScriptHost& host) {
    sol::table physics = lua.create_table();

    physics["raycast"] = [&host](const glm::vec3& origin, const glm::vec3& dir,
                                 sol::optional<float> maxDist,
                                 sol::this_state s) -> sol::object {
        sol::state_view sv(s);
        Scene* sc = host.activeScene();
        AssetCache* ac = host.context().assets;
        if (!sc || !ac) return sol::make_object(sv, sol::lua_nil);
        glm::vec3 d = dir;
        if (glm::length(d) > 1e-8f) d = glm::normalize(d);
        auto hit = raycastScene(*sc, *ac, origin, d, maxDist.value_or(0.0f));
        if (!hit) return sol::make_object(sv, sol::lua_nil);
        sol::table out = sv.create_table();
        out["entity"] = Entity(hit->entity, sc);
        out["point"] = hit->point;
        out["normal"] = hit->normal;
        out["distance"] = hit->distance;
        return out;
    };

    physics["overlap"] = [&host](Entity a, Entity b) -> bool {
        Scene* sc = host.activeScene();
        AssetCache* ac = host.context().assets;
        if (!sc || !ac) return false;
        entt::registry& reg = sc->registry();

        // World AABB: transform the 8 corners of the local box (Collider when
        // non-degenerate, else mesh bounds) and take the min/max.
        auto worldAabb =
            [&](Entity e) -> std::optional<std::pair<glm::vec3, glm::vec3>> {
            const Transform* t = reg.try_get<Transform>(e.handle());
            if (!t) return std::nullopt;
            glm::vec3 lo, hi;
            bool haveBox = false;
            if (const Collider* c = reg.try_get<Collider>(e.handle())) {
                if (c->halfExtents.x != 0.0f || c->halfExtents.y != 0.0f ||
                    c->halfExtents.z != 0.0f) {
                    lo = c->center - c->halfExtents;
                    hi = c->center + c->halfExtents;
                    haveBox = true;
                }
            }
            if (!haveBox) {
                if (const MeshRenderer* mr =
                        reg.try_get<MeshRenderer>(e.handle())) {
                    if (const Mesh* mesh = ac->mesh(mr->meshAsset)) {
                        lo = mesh->localMin;
                        hi = mesh->localMax;
                        haveBox = true;
                    }
                }
            }
            if (!haveBox) return std::nullopt;
            const glm::mat4 m = t->matrix();
            glm::vec3 wmin(1e30f), wmax(-1e30f);
            for (int i = 0; i < 8; ++i) {
                glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                                 (i & 4) ? hi.z : lo.z);
                glm::vec3 w(m * glm::vec4(corner, 1.0f));
                wmin = glm::min(wmin, w);
                wmax = glm::max(wmax, w);
            }
            return std::make_pair(wmin, wmax);
        };

        auto ba = worldAabb(a);
        auto bb = worldAabb(b);
        if (!ba || !bb) return false;
        const glm::vec3& aMin = ba->first;
        const glm::vec3& aMax = ba->second;
        const glm::vec3& bMin = bb->first;
        const glm::vec3& bMax = bb->second;
        return aMin.x <= bMax.x && aMax.x >= bMin.x && aMin.y <= bMax.y &&
               aMax.y >= bMin.y && aMin.z <= bMax.z && aMax.z >= bMin.z;
    };

    lm["physics"] = physics;
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
    lm["import"] = [&host](const std::string& path) -> sol::object {
        return host.importModule(path);
    };
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
            // Store a Lua-OWNED copy: assigning the lvalue `e` would push a
            // reference to this stack local, which dangles once the callback
            // returns. make_object copies the value handle into Lua.
            if (n.value == name) out[i++] = sol::make_object(sv, e);
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
            // Pass an rvalue so sol pushes a Lua-OWNED copy of the value
            // handle. Passing the lvalue `e` would hand Lua a reference to this
            // stack local; a callback that stashes the entity (e.g. collect-
            // then-destroy) would then dereference freed stack memory next
            // iteration — the Maze editor SIGSEGV in Entity::valid.
            sol::protected_function_result r = fn(Entity(h, sc));
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

    sol::table json = lua.create_table();
    json["encode"] = [](const sol::object& value) -> std::string {
        return luaToJson(value).dump();
    };
    json["decode"] = [](const std::string& text,
                        sol::this_state s) -> sol::object {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(text);
        } catch (const nlohmann::json::parse_error& e) {
            throw sol::error(std::string("lm.json.decode: ") + e.what());
        }
        return jsonToLua(sol::state_view(s), parsed);
    };
    lm["json"] = json;

    bindAudio(lm, lua, host);
    bindAssets(lm, lua, host);
    bindRender(lm, lua, host);
    bindUi(lm, lua, host);
    bindPhysics(lm, lua, host);
    bindProcgen(lm, lua);
#if defined(LIMINAL_WITH_INFERENCE)
    bindAi(lm, lua, host);
#endif
}

} // namespace liminal::luabind
