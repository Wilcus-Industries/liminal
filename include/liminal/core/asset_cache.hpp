#pragma once
// AssetCache: string name -> GPU resource, created on first use, owned for
// the cache's lifetime. This is what lets MeshRenderer hold plain strings
// (trivially serializable) while the App's render system still gets Mesh* /
// Texture* every frame.
//
// Naming convention ("builtin:" needs no files on disk):
//   meshes:   builtin:box  builtin:pyramid  builtin:pillar  builtin:arch
//             builtin:stair  builtin:plane  builtin:quad
//             seeded: builtin:blob:<seed>  builtin:tree:<seed>
//                     builtin:rock:<seed>  builtin:crystal:<seed>
//   textures: builtin:white  builtin:checker  builtin:grid  builtin:noise
//             builtin:concrete  builtin:wood  builtin:metal  builtin:brick
//             builtin:plaster  builtin:grass  builtin:dirt  builtin:water
//             (procedural, neutral grays — tint via MeshRenderer.color)
//             anything else: a file path resolved via Assets::resolve and
//             loaded with Texture::fromFile (PNG etc.).
//
// There is no mesh file loader yet, so a non-builtin mesh name is unknown.
// Unknown names warn once on stderr and return nullptr; callers skip the draw.
// Requires a current GL context (construct after Window).

#include <string>
#include <unordered_map>

#include <liminal/render/mesh.hpp>
#include <liminal/render/texture.hpp>

namespace liminal {

class AssetCache {
public:
    AssetCache() = default;
    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    // nullptr if the name is unknown/unloadable (warned once per name).
    const Mesh* mesh(const std::string& name);
    const Texture* texture(const std::string& name);

    // Registers (or overwrites) a runtime-built mesh. The storage key is
    // "runtime:" + name unless name already begins with "runtime:". Any
    // warn-once memo for that key is cleared so a previously-unknown name now
    // resolves. Returns the storage key (look the mesh up with mesh(key) if you
    // need the pointer). Used by lm.assets.add_mesh so scripts can feed
    // procedural geometry to the cache and get back the resolvable name.
    std::string addMesh(const std::string& name, const MeshData& data);

private:
    std::unordered_map<std::string, Mesh> m_meshes;
    std::unordered_map<std::string, Texture> m_textures;
    std::unordered_map<std::string, bool> m_failed; // warn-once memo
};

} // namespace liminal
