// Name -> GPU resource cache (convention documented in the header).
#include <liminal/core/asset_cache.hpp>

#include <liminal/core/assets.hpp>

#include <cstdio>
#include <cstdlib>
#include <optional>

namespace liminal {

namespace {

constexpr const char* kBuiltinPrefix = "builtin:";

bool isBuiltin(const std::string& name) {
    return name.rfind(kBuiltinPrefix, 0) == 0;
}

// Splits "blob:42" -> {"blob", 42}; bare names get seed 0.
void splitSeed(const std::string& body, std::string& kind, unsigned int& seed) {
    const auto colon = body.find(':');
    if (colon == std::string::npos) {
        kind = body;
        seed = 0;
        return;
    }
    kind = body.substr(0, colon);
    seed = static_cast<unsigned int>(
        std::strtoul(body.c_str() + colon + 1, nullptr, 10));
}

std::optional<Mesh> makeBuiltinMesh(const std::string& body) {
    std::string kind;
    unsigned int seed = 0;
    splitSeed(body, kind, seed);
    if (kind == "box") return Mesh::box();
    if (kind == "pyramid") return Mesh::pyramid();
    if (kind == "pillar") return Mesh::pillar();
    if (kind == "arch") return Mesh::arch();
    if (kind == "stair") return Mesh::stair();
    if (kind == "plane") return Mesh::plane();
    if (kind == "quad") return Mesh::quad();
    if (kind == "blob") return Mesh::blob(seed);
    if (kind == "tree") return Mesh::tree(seed);
    if (kind == "rock") return Mesh::rock(seed);
    if (kind == "crystal") return Mesh::crystal(seed);
    return std::nullopt;
}

std::optional<Texture> makeBuiltinTexture(const std::string& body) {
    std::string kind;
    unsigned int seed = 0;
    splitSeed(body, kind, seed);
    // Neutral grays — MeshRenderer.color does the tinting.
    const glm::vec3 light{0.78f};
    const glm::vec3 dark{0.5f};
    using P = Texture::Procedural;
    const int size = 64;
    if (kind == "white") return Texture::procedural(P::White, 1, light, dark, seed);
    if (kind == "grid") return Texture::procedural(P::Grid, size, light, dark, seed);
    if (kind == "checker") return Texture::procedural(P::Checker, size, light, dark, seed);
    if (kind == "noise") return Texture::procedural(P::Noise, size, light, dark, seed);
    if (kind == "concrete") return Texture::procedural(P::Concrete, size, light, dark, seed);
    if (kind == "wood") return Texture::procedural(P::Wood, size, light, dark, seed);
    if (kind == "metal") return Texture::procedural(P::Metal, size, light, dark, seed);
    if (kind == "brick") return Texture::procedural(P::Brick, size, light, dark, seed);
    if (kind == "plaster") return Texture::procedural(P::Plaster, size, light, dark, seed);
    if (kind == "grass") return Texture::procedural(P::Grass, size, light, dark, seed);
    if (kind == "dirt") return Texture::procedural(P::Dirt, size, light, dark, seed);
    if (kind == "water") return Texture::procedural(P::Water, size, light, dark, seed);
    return std::nullopt;
}

} // namespace

const Mesh* AssetCache::mesh(const std::string& name) {
    if (name.empty()) return nullptr;
    if (auto it = m_meshes.find(name); it != m_meshes.end()) return &it->second;
    if (m_failed.count(name)) return nullptr;

    std::optional<Mesh> built;
    if (isBuiltin(name)) {
        built = makeBuiltinMesh(name.substr(std::string(kBuiltinPrefix).size()));
    }
    // No mesh file loader exists yet, so non-builtin names can't resolve.
    if (!built) {
        std::fprintf(stderr, "liminal: AssetCache: unknown mesh \"%s\"\n",
                     name.c_str());
        m_failed.emplace(name, true);
        return nullptr;
    }
    auto [it, _] = m_meshes.emplace(name, std::move(*built));
    return &it->second;
}

std::string AssetCache::addMesh(const std::string& name, const MeshData& data) {
    constexpr const char* kRuntimePrefix = "runtime:";
    std::string key =
        name.rfind(kRuntimePrefix, 0) == 0 ? name : kRuntimePrefix + name;
    m_failed.erase(key); // a name that failed to resolve before now exists
    auto [it, inserted] = m_meshes.try_emplace(key, Mesh(data));
    if (!inserted) it->second = Mesh(data); // overwrite an existing runtime mesh
    return key;
}

const Texture* AssetCache::texture(const std::string& name) {
    if (name.empty()) return nullptr;
    if (auto it = m_textures.find(name); it != m_textures.end()) return &it->second;
    if (m_failed.count(name)) return nullptr;

    std::optional<Texture> built;
    if (isBuiltin(name)) {
        built = makeBuiltinTexture(name.substr(std::string(kBuiltinPrefix).size()));
    } else if (auto bytes = Assets::readFile(name)) {
        // Routed through the VFS so a mounted pak serves the image; on disk
        // (editor) it's the same bytes a plain file read would yield.
        built = Texture::fromMemory(
            reinterpret_cast<const unsigned char*>(bytes->data()), bytes->size());
    }
    if (!built) {
        std::fprintf(stderr, "liminal: AssetCache: cannot load texture \"%s\"\n",
                     name.c_str());
        m_failed.emplace(name, true);
        return nullptr;
    }
    auto [it, _] = m_textures.emplace(name, std::move(*built));
    return &it->second;
}

} // namespace liminal
