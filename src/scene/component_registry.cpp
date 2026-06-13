// ComponentRegistry instance + the built-in component registrations: each
// built-in gets a stable name, a to/from-JSON pair, and an ImGui inspector
// (consumed by the Phase 6 editor; harmless without it).
#include <liminal/scene/component_registry.hpp>

#include <liminal/scene/components.hpp>
#include <liminal/scene/serialize.hpp>

#include <imgui.h>

// Defined in src/script/lua_bindings.cpp. Declared here instead of including
// the sol2-heavy header so this file stays scripting-agnostic.
namespace liminal::luabind {
void bindName(void* luaState);
void bindTransform(void* luaState);
void bindMeshRenderer(void* luaState);
} // namespace liminal::luabind
#define LIMINAL_LUABIND(fn) (&liminal::luabind::fn)

namespace liminal {

ComponentRegistry& ComponentRegistry::instance() {
    static ComponentRegistry registry;
    return registry;
}

void ComponentRegistry::registerOps(ComponentOps ops) {
    for (auto& existing : m_ops) {
        if (existing.name == ops.name) {
            existing = std::move(ops); // re-register = overwrite
            return;
        }
    }
    m_ops.push_back(std::move(ops));
}

const ComponentOps* ComponentRegistry::find(const std::string& name) const {
    for (const auto& ops : m_ops) {
        if (ops.name == name) return &ops;
    }
    return nullptr;
}

namespace {

// json.value() with the component's default keeps load tolerant of files
// written by older versions that lacked a field.

void registerName(ComponentRegistry& r) {
    r.registerComponent<Name>(
        "Name",
        [](const Name& c) { return nlohmann::json{{"value", c.value}}; },
        [](Name& c, const nlohmann::json& j) { c.value = j.value("value", ""); },
        [](Name& c) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", c.value.c_str());
            if (ImGui::InputText("name", buf, sizeof(buf))) c.value = buf;
        },
        LIMINAL_LUABIND(bindName));
}

void registerTransform(ComponentRegistry& r) {
    r.registerComponent<Transform>(
        "Transform",
        [](const Transform& c) {
            return nlohmann::json{{"position", c.position},
                                  {"rotationEuler", c.rotationEuler},
                                  {"scale", c.scale}};
        },
        [](Transform& c, const nlohmann::json& j) {
            c.position = j.value("position", glm::vec3(0.0f));
            c.rotationEuler = j.value("rotationEuler", glm::vec3(0.0f));
            c.scale = j.value("scale", glm::vec3(1.0f));
        },
        [](Transform& c) {
            ImGui::DragFloat3("position", &c.position.x, 0.05f);
            ImGui::DragFloat3("rotation (deg)", &c.rotationEuler.x, 1.0f);
            ImGui::DragFloat3("scale", &c.scale.x, 0.05f);
        },
        LIMINAL_LUABIND(bindTransform));
}

void registerMeshRenderer(ComponentRegistry& r) {
    r.registerComponent<MeshRenderer>(
        "MeshRenderer",
        [](const MeshRenderer& c) {
            return nlohmann::json{{"meshAsset", c.meshAsset},
                                  {"color", c.color},
                                  {"textureAsset", c.textureAsset}};
        },
        [](MeshRenderer& c, const nlohmann::json& j) {
            c.meshAsset = j.value("meshAsset", "builtin:box");
            c.color = j.value("color", glm::vec4(1.0f));
            c.textureAsset = j.value("textureAsset", "");
        },
        [](MeshRenderer& c) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", c.meshAsset.c_str());
            if (ImGui::InputText("mesh", buf, sizeof(buf))) c.meshAsset = buf;
            std::snprintf(buf, sizeof(buf), "%s", c.textureAsset.c_str());
            if (ImGui::InputText("texture", buf, sizeof(buf))) c.textureAsset = buf;
            ImGui::ColorEdit4("color", &c.color.x);
        },
        LIMINAL_LUABIND(bindMeshRenderer));
}

void registerCamera(ComponentRegistry& r) {
    r.registerComponent<Camera>(
        "Camera",
        [](const Camera& c) {
            return nlohmann::json{{"fovDeg", c.fovDeg},
                                  {"nearZ", c.nearZ},
                                  {"farZ", c.farZ},
                                  {"primary", c.primary}};
        },
        [](Camera& c, const nlohmann::json& j) {
            c.fovDeg = j.value("fovDeg", 70.0f);
            c.nearZ = j.value("nearZ", 0.1f);
            c.farZ = j.value("farZ", 220.0f);
            c.primary = j.value("primary", true);
        },
        [](Camera& c) {
            ImGui::SliderFloat("fov", &c.fovDeg, 30.0f, 120.0f);
            ImGui::DragFloat("near", &c.nearZ, 0.01f, 0.001f, 10.0f);
            ImGui::DragFloat("far", &c.farZ, 1.0f, 10.0f, 2000.0f);
            ImGui::Checkbox("primary", &c.primary);
        });
}

void registerAudioSource(ComponentRegistry& r) {
    r.registerComponent<AudioSource>(
        "AudioSource",
        [](const AudioSource& c) {
            return nlohmann::json{{"gain", c.gain}, {"enabled", c.enabled}};
        },
        [](AudioSource& c, const nlohmann::json& j) {
            c.gain = j.value("gain", 0.14f);
            c.enabled = j.value("enabled", true);
        },
        [](AudioSource& c) {
            ImGui::SliderFloat("gain", &c.gain, 0.0f, 1.0f);
            ImGui::Checkbox("enabled", &c.enabled);
        });
}

void registerLight(ComponentRegistry& r) {
    r.registerComponent<Light>(
        "Light",
        [](const Light& c) {
            return nlohmann::json{{"color", c.color}, {"intensity", c.intensity}};
        },
        [](Light& c, const nlohmann::json& j) {
            c.color = j.value("color", glm::vec3(1.0f));
            c.intensity = j.value("intensity", 1.0f);
        },
        [](Light& c) {
            ImGui::ColorEdit3("color", &c.color.x);
            ImGui::SliderFloat("intensity", &c.intensity, 0.0f, 10.0f);
        });
}

void registerScript(ComponentRegistry& r) {
    r.registerComponent<Script>(
        "Script",
        [](const Script& c) { return nlohmann::json{{"paths", c.paths}}; },
        [](Script& c, const nlohmann::json& j) {
            if (j.contains("paths") && j["paths"].is_array()) {
                c.paths.clear();
                for (const auto& p : j["paths"]) c.paths.push_back(p.get<std::string>());
            } else if (j.contains("path") && j["path"].is_string()) {
                // Legacy single-path .lscene; promote to a one-element list.
                c.paths = {j["path"].get<std::string>()};
            } else {
                c.paths.clear();
            }
        },
        [](Script& c) {
            int removeIdx = -1;
            for (std::size_t i = 0; i < c.paths.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", c.paths[i].c_str());
                if (ImGui::InputText("path", buf, sizeof(buf))) c.paths[i] = buf;
                ImGui::SameLine();
                if (ImGui::Button("X")) removeIdx = static_cast<int>(i);
                ImGui::PopID();
            }
            if (removeIdx >= 0)
                c.paths.erase(c.paths.begin() + removeIdx);
            if (ImGui::Button("Add script")) c.paths.emplace_back();
        });
}

} // namespace

void registerBuiltinComponents() {
    static bool done = false;
    if (done) return;
    done = true;
    ComponentRegistry& r = ComponentRegistry::instance();
    registerName(r);
    registerTransform(r);
    registerMeshRenderer(r);
    registerCamera(r);
    registerAudioSource(r);
    registerLight(r);
    registerScript(r);
}

} // namespace liminal
