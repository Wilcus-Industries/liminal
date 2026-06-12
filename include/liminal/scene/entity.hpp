#pragma once
// Entity: a cheap {entt::entity, Scene*} value handle. Copy it freely; it owns
// nothing. Member templates are defined at the bottom of scene.hpp (they need
// the full Scene), so include <liminal/scene/scene.hpp> to use them — this
// header exists for forward-declaration-ish call sites that only pass handles
// around.

#include <entt/entt.hpp>

namespace liminal {

class Scene;

class Entity {
public:
    Entity() = default;
    Entity(entt::entity handle, Scene* scene) : m_handle(handle), m_scene(scene) {}

    // Adds component T constructed from args; returns a reference to it.
    // Replaces an existing T (entt emplace_or_replace semantics).
    template <typename T, typename... Args>
    T& add(Args&&... args);
    // Designated-initializer form: e.add<Transform>({.position = {0,1,0}}).
    template <typename T>
    T& add(T component);

    template <typename T> T& get();
    template <typename T> const T& get() const;
    template <typename T> bool has() const;
    template <typename T> void remove();

    void destroy(); // removes the entity from its scene; handle goes invalid

    bool valid() const;
    explicit operator bool() const { return valid(); }
    bool operator==(const Entity& o) const {
        return m_handle == o.m_handle && m_scene == o.m_scene;
    }

    entt::entity handle() const { return m_handle; }
    Scene* scene() const { return m_scene; }

private:
    entt::entity m_handle = entt::null;
    Scene* m_scene = nullptr;
};

} // namespace liminal
