// .lscene serialization (format documented in <liminal/scene/serialize.hpp>).
#include <liminal/scene/serialize.hpp>

#include <liminal/scene/component_registry.hpp>
#include <liminal/scene/scene.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace glm {

void to_json(nlohmann::json& j, const vec3& v) { j = {v.x, v.y, v.z}; }
void from_json(const nlohmann::json& j, vec3& v) {
    v = {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()};
}
void to_json(nlohmann::json& j, const vec4& v) { j = {v.x, v.y, v.z, v.w}; }
void from_json(const nlohmann::json& j, vec4& v) {
    v = {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>(),
         j.at(3).get<float>()};
}

} // namespace glm

namespace liminal {

namespace {
constexpr int kSceneVersion = 1;
} // namespace

nlohmann::json sceneToJson(const Scene& scene) {
    registerBuiltinComponents();
    const auto& registry = scene.registry();
    const auto& ops = ComponentRegistry::instance().all();

    // Stable output: entities sorted by entt id (view order is storage
    // order, which save order shouldn't depend on).
    std::vector<entt::entity> entities;
    for (auto e : registry.view<entt::entity>()) entities.push_back(e);
    std::sort(entities.begin(), entities.end(),
              [](entt::entity a, entt::entity b) {
                  return entt::to_integral(a) < entt::to_integral(b);
              });

    nlohmann::json out;
    out["liminal_scene"] = kSceneVersion;
    out["entities"] = nlohmann::json::array();
    for (auto e : entities) {
        nlohmann::json components = nlohmann::json::object();
        for (const auto& op : ops) {
            if (op.has(registry, e)) components[op.name] = op.toJson(registry, e);
        }
        out["entities"].push_back(
            {{"id", entt::to_integral(e)}, {"components", components}});
    }
    return out;
}

void sceneFromJson(Scene& scene, const nlohmann::json& j) {
    registerBuiltinComponents();
    if (j.value("liminal_scene", -1) != kSceneVersion) {
        throw std::runtime_error("not a liminal_scene v1 document");
    }
    const auto& registry = ComponentRegistry::instance();
    const nlohmann::json entities = j.value("entities", nlohmann::json::array());
    for (const auto& entry : entities) {
        Entity e = scene.create();
        // Bind before .items(): iterating a temporary's items() dangles.
        const nlohmann::json components =
            entry.value("components", nlohmann::json::object());
        for (const auto& [name, body] : components.items()) {
            const ComponentOps* ops = registry.find(name);
            if (!ops) {
                std::fprintf(stderr,
                             "liminal: scene load: unknown component \"%s\" — skipped\n",
                             name.c_str());
                continue;
            }
            ops->fromJson(scene.registry(), e.handle(), body);
        }
    }
}

bool Scene::save(const std::string& path) const {
    std::ofstream file(path);
    if (!file) {
        std::fprintf(stderr, "liminal: scene save: cannot write \"%s\"\n",
                     path.c_str());
        return false;
    }
    file << sceneToJson(*this).dump(2) << '\n';
    return file.good();
}

Scene Scene::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("liminal: scene load: cannot read \"" + path + "\"");
    }
    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception& ex) {
        throw std::runtime_error("liminal: scene load: \"" + path + "\": " + ex.what());
    }
    Scene scene;
    sceneFromJson(scene, j);
    return scene;
}

} // namespace liminal
