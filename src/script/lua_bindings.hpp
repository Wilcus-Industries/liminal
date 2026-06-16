#pragma once
// Internal glue between ScriptHost and the lm.* Lua API. Not installed in
// the public include tree — component_registry.cpp forward-declares the
// per-component binders itself (it must compile with scripting OFF).

#include <string>

#include <sol/sol.hpp>

namespace liminal {

class ScriptHost;

namespace luabind {

// Binds vec3/vec4, the Entity usertype, and the global lm table (log, scene,
// input, time). Called once from the ScriptHost constructor.
void bindCore(sol::state& lua, ScriptHost& host);

// Binds lm.procgen.* — the full procgen toolkit (rng, tileset, terrain, WFC,
// validation, shape grammar, plus a one-shot town() that mirrors the
// 04_wfc_town example). Lives in its own translation unit (lua_bindings_
// procgen.cpp) to keep the heavy procgen includes off lua_bindings.cpp.
// Called from bindCore.
void bindProcgen(sol::table& lm, sol::state& lua);

// entity:get_component("Name") support: each component binder registers a
// pusher keyed by its registry name; this turns a raw T* (from
// ComponentOps::getRaw) into a Lua userdata referencing it. Returns nil for
// names without a pusher.
sol::object pushComponent(lua_State* L, const std::string& name, void* raw);

// ComponentOps::luaBind targets for the built-ins (void* = sol::state*).
void bindName(void* luaState);
void bindTransform(void* luaState);
void bindMeshRenderer(void* luaState);
void bindCamera(void* luaState);
void bindLight(void* luaState);
void bindCollider(void* luaState);
void bindBillboard(void* luaState);

} // namespace luabind
} // namespace liminal
