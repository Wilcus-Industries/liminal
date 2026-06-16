#pragma once
// Procedural GL textures plus stb_image file loading (NPC sprites).
// Nearest-neighbor filtering everywhere — bilinear smoothing would kill
// the PS1 look.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace liminal {

class Texture {
public:
    enum class Procedural {
        Grid,     // colorB lines over colorA fill
        Checker,  // colorA/colorB checkerboard
        Noise,    // value noise between colorA and colorB
        White,    // 1px white — lets uColor tint do all the work
        // v3 material themes. All are built from a single brightness field
        // mixed colorB (dark) -> colorA (light) and Bayer-dithered to a few
        // levels, so neutral-gray inputs give tintable PS1-grade surfaces.
        Concrete, // blotchy slab + sparse dark pits
        Wood,     // vertical planks, per-plank shade, dark seams + grain
        Metal,    // corrugated vertical bands + rust speckles
        Brick,    // running bond, light mortar lines, per-brick shade
        Plaster,  // near-flat field with faint large stains
        Grass,    // dense vertical flecks
        Dirt,     // coarse blotches with per-texel grain
        Water,    // horizontal wavy bands
    };

    static Texture procedural(Procedural kind, int size,
                              const glm::vec3& colorA, const glm::vec3& colorB,
                              unsigned int seed = 0);

    // Procedural creepy decals/billboards. Unlike procedural() these carry a
    // real alpha channel: the field is mostly transparent (alpha 0) and gets
    // discarded by the shader's alpha-test, so the sprite reads as a cutout
    // stamp rather than a tiling pattern. Pareidolia does the rest of the work
    // — the brain insists on seeing a face/figure in the muted noise.
    enum class Sprite {
        Face,    // x-symmetric eye sockets + mouth over a noisy field
        Sigil,   // a few runic strokes on a transparent field
        Stain,   // directional noise blotch, alpha fading toward the edges
        Figure,  // tall thin dark humanoid silhouette, transparent around it
        FigureEyes, // same body (same seed -> same pose) + two pale eye points
        Scrawl,  // rows of rune-like garbled "text"
        Handprint, // palm blob + five finger strokes, smeared at the edges
        Eyes,    // 1-3 pairs of pale points in dark sockets, nothing else
        Hanging, // thin line from the top edge ending in a slumped dark mass
        Drip,    // blotch at the top bleeding into tapering vertical runs
    };

    // Builds a sprite from 1-2 colors hashed out of `palette` (muted gray
    // fallback if empty). Deterministic in `seed` down to the byte.
    static Texture sprite(Sprite kind, int size,
                          const std::vector<glm::vec3>& palette,
                          unsigned int seed);

    // Loads an RGBA image file (PNG etc.) via stb_image as a single-stamp
    // texture: nearest filtering, CLAMP_TO_EDGE — same contract as sprite().
    // Empty optional on read/decode failure.
    static std::optional<Texture> fromFile(const std::string& path);

    // Decode an in-memory image (PNG etc.) the same way fromFile does — same
    // nearest/CLAMP_TO_EDGE contract — but from a byte buffer rather than a
    // path, so a pak-served texture decodes without ever touching disk.
    // Empty optional on decode failure.
    static std::optional<Texture> fromMemory(const unsigned char* bytes,
                                             std::size_t len);

    // Builds an RGBA8 texture from a raw pixel buffer (w*h*4 bytes, row-major,
    // top-left origin). Same nearest / CLAMP_TO_EDGE contract as fromMemory —
    // backs lm.assets.add_texture(name, w, h, pixels). Empty optional if w/h <= 0.
    static std::optional<Texture> fromPixels(int w, int h,
                                             const unsigned char* rgba);

    // Width / height of the source image (billboard quads scale by this).
    // 1.0 for the square procedural textures.
    float aspect() const { return m_aspect; }

    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    void bind(int unit) const;

private:
    Texture() = default;

    // Shared GL upload tail for the CPU-built RGBA8 buffers above. `wrap` is
    // GL_REPEAT for tiling patterns or GL_CLAMP_TO_EDGE for single stamps.
    // Always nearest min+mag, no mipmaps (see Texture.cpp header for why).
    static Texture upload(const std::vector<unsigned char>& pixels, int size,
                          int wrap);
    // Rectangular variant; upload(size) forwards here. Raw pointer so the
    // stb buffer can be passed without a copy.
    static Texture uploadWH(const unsigned char* pixels, int w, int h,
                            int wrap);

    unsigned int m_id = 0;
    float m_aspect = 1.0f;
};

} // namespace liminal
