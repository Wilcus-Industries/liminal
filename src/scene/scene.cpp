#include <liminal/scene/scene.hpp>

namespace liminal {

Entity Scene::create(const std::string& name) {
    const entt::entity e = m_registry.create();
    if (!name.empty()) m_registry.emplace<Name>(e, name);
    return Entity(e, this);
}

void Scene::destroy(Entity e) {
    if (e.scene() == this && m_registry.valid(e.handle())) {
        m_registry.destroy(e.handle());
    }
}

Entity Scene::find(const std::string& name) {
    for (auto [entity, n] : m_registry.view<Name>().each()) {
        if (n.value == name) return Entity(entity, this);
    }
    return Entity();
}

std::size_t Scene::entityCount() const {
    std::size_t count = 0;
    for ([[maybe_unused]] auto e : m_registry.view<entt::entity>()) ++count;
    return count;
}

} // namespace liminal
