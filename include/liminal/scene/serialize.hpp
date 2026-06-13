#pragma once
// Scene JSON serialization — the .lscene format. The entry points live on
// Scene (scene.save(path) / Scene::load(path)); this header exposes the
// in-memory halves plus the glm <-> json adapters the built-in component
// serializers use.
//
// Format (version 1):
//   {
//     "liminal_scene": 1,
//     "entities": [
//       { "id": 0,
//         "components": {
//           "Name":      { "value": "crate" },
//           "Transform": { "position": [0,1,0], "rotationEuler": [0,0,0],
//                          "scale": [1,1,1] }
//         } }
//     ]
//   }
//
// - Entities are written sorted by entt id, components keyed by registry
//   name; nlohmann objects keep keys sorted — save/load/save is body-stable.
// - "id" records the source entt id for cross-references/debugging; load()
//   creates fresh entities in file order and does NOT promise id
//   preservation (a freshly built scene round-trips to the same ids).
// - Unknown component names warn on stderr and are skipped, never fatal.
// - Asset references (MeshRenderer.meshAsset/.textureAsset) stay strings;
//   GPU resources bind lazily through AssetCache when the App renders.

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

namespace liminal {

class Scene;

// Scene -> json / json -> entities-in-scene. sceneFromJson appends into an
// empty scene; throws std::runtime_error on a missing/wrong "liminal_scene"
// version tag.
nlohmann::json sceneToJson(const Scene& scene);
void sceneFromJson(Scene& scene, const nlohmann::json& j);

// Parse a .lscene JSON document from a string into a fresh Scene. `nameForErrors`
// is the source name (file path or pak key) used in thrown error messages.
// Throws std::runtime_error on parse failure or a missing/wrong version tag.
// Scene::load(path) is this plus an Assets::readFile front end.
Scene loadFromString(const std::string& json, const std::string& nameForErrors);

} // namespace liminal

// glm adapters in glm's namespace so nlohmann finds them by ADL.
namespace glm {
void to_json(nlohmann::json& j, const vec3& v);
void from_json(const nlohmann::json& j, vec3& v);
void to_json(nlohmann::json& j, const vec4& v);
void from_json(const nlohmann::json& j, vec4& v);
} // namespace glm
