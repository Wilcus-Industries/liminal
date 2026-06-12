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

#include <liminal/core/window.hpp>
#include <liminal/scene/component_registry.hpp>
#include <liminal/scene/scene.hpp>
#include <liminal/script/script_host.hpp>

#include <cctype>
#include <cstdio>
#include <unordered_map>

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

} // namespace

void bindCore(sol::state& lua, ScriptHost& host) {
    bindVecTypes(lua);
    bindEntity(lua);

    sol::table lm = lua.create_named_table("lm");
    lm["log"] = [](const std::string& msg) {
        std::printf("[lua] %s\n", msg.c_str());
        std::fflush(stdout);
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
    lm["input"] = input;

    sol::table time = lua.create_table();
    time["now"] = [&host]() { return host.now(); };
    lm["time"] = time;
}

} // namespace liminal::luabind
