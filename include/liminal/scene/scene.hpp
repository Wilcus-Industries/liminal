#pragma once
// Scene: a thin, friendly facade over entt::registry. Flat entity list (no
// parenting this phase), name lookup, typed iteration, and JSON save/load
// driven by the ComponentRegistry. The registry() escape hatch hands back the
// raw entt registry when the facade is in the way.
//
//   liminal::Scene scene;
//   liminal::Entity e = scene.create("crate");        // Name auto-added
//   e.add<liminal::Transform>({.position = {0, 1, 0}});
//   scene.each<liminal::Transform, liminal::MeshRenderer>(
//       [](liminal::Entity, liminal::Transform& t, liminal::MeshRenderer& m) {});
//   scene.save("level.lscene");
//   liminal::Scene back = liminal::Scene::load("level.lscene");

#include <cassert>
#include <string>
#include <utility>

#include <entt/entt.hpp>

#include <liminal/scene/components.hpp>
#include <liminal/scene/entity.hpp>

namespace liminal {

class Scene {
public:
    Scene() = default;
    Scene(Scene&&) noexcept = default;
    Scene& operator=(Scene&&) noexcept = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Creates an entity; a Name component is added when name is non-empty.
    Entity create(const std::string& name = {});
    void destroy(Entity e);

    // First entity whose Name matches; invalid Entity if none. Linear scan —
    // fine at scene scale, cache the handle if you call it per frame.
    Entity find(const std::string& name);

    // Calls fn(Entity, Cs&...) for every entity owning all of Cs.
    template <typename... Cs, typename Fn>
    void each(Fn&& fn) {
        for (auto entity : m_registry.view<Cs...>()) {
            fn(Entity(entity, this), m_registry.get<Cs>(entity)...);
        }
    }

    std::size_t entityCount() const;

    // Serialization (implemented in serialize.cpp; format documented in
    // <liminal/scene/serialize.hpp>). save() writes every component type the
    // ComponentRegistry knows about; load() warns + skips unknown names.
    // save() returns false (with a stderr warning) when the file can't be
    // written; load() throws std::runtime_error when the file can't be read
    // or isn't a .lscene document.
    bool save(const std::string& path) const;
    static Scene load(const std::string& path);

    entt::registry& registry() { return m_registry; }
    const entt::registry& registry() const { return m_registry; }

private:
    entt::registry m_registry;
};

// --- Entity member templates (need the full Scene) --------------------------

template <typename T, typename... Args>
T& Entity::add(Args&&... args) {
    assert(m_scene && "Entity::add on entity with no scene");
    return m_scene->registry().emplace_or_replace<T>(m_handle,
                                                     std::forward<Args>(args)...);
}

template <typename T>
T& Entity::add(T component) {
    assert(m_scene && "Entity::add on entity with no scene");
    return m_scene->registry().emplace_or_replace<T>(m_handle, std::move(component));
}

template <typename T>
T& Entity::get() {
    assert(m_scene && "Entity::get on entity with no scene");
    return m_scene->registry().get<T>(m_handle);
}

template <typename T>
const T& Entity::get() const {
    assert(m_scene && "Entity::get on entity with no scene");
    return m_scene->registry().get<T>(m_handle);
}

template <typename T>
bool Entity::has() const {
    return m_scene && m_scene->registry().all_of<T>(m_handle);
}

template <typename T>
void Entity::remove() {
    if (m_scene) m_scene->registry().remove<T>(m_handle);
}

inline void Entity::destroy() {
    if (valid()) m_scene->destroy(*this);
    m_handle = entt::null;
}

inline bool Entity::valid() const {
    return m_scene != nullptr && m_scene->registry().valid(m_handle);
}

} // namespace liminal
