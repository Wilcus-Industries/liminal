// Texture.cpp — procedural RGBA8 textures.
//
// Everything here is built on the CPU as a flat byte buffer and uploaded
// once. The aesthetic constraints drive the GL state:
//   - GL_NEAREST mag and GL_NEAREST_MIPMAP_NEAREST min — point-sampled at
//     every stage. Bilinear/trilinear smears texels together; the PS1 sampled
//     nearest and that hard texel edge is most of the look. The mip variant
//     stays nearest (no bilinear) but selects a mip sized to the on-screen
//     footprint, so minifying a large texture reads coherent texels instead
//     of thrashing the GPU cache (was a hard FPS cliff with a plain
//     GL_NEAREST min filter). Because the min filter samples mips,
//     glGenerateMipmap MUST run after upload — a mipmapping min filter with
//     no mip levels is "incomplete" and samples black, a classic GL gotcha.
//   - Wrap mode is per-use: procedural() patterns tile across big surfaces
//     (ground plane especially) so they use GL_REPEAT and are authored to
//     wrap seamlessly; sprite() decals are single stamps and use
//     GL_CLAMP_TO_EDGE so a billboard never bleeds its opposite edge in.
//
// procedural() forces alpha to 255 (opaque) — phase 1 has no transparency.
// sprite() is the exception: it builds a meaningful alpha cutout (0 = gone,
// 255 = solid) that the fragment shader alpha-tests against, since the whole
// renderer runs with GL_BLEND off (the hard cutout edge is on-theme anyway).

#include <liminal/render/texture.hpp>

#include "hash_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <glad/gl.h>

#define STB_IMAGE_IMPLEMENTATION
// Whitelist the formats buildGamePak ships (see src/core/pak.cpp). stb treats
// the STBI_ONLY_* set as an allowlist; without JPEG/TGA/BMP, paks carrying
// those would fail to decode at runtime.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include <stb_image.h>

namespace liminal {

namespace {

// Cheap integer avalanche hash (lowbias32 variant). Deterministic across
// runs/platforms, which matters: the same dream seed must hallucinate the
// same noise texture every time it recurs.
using render_detail::hashU32;

// Hash of a (x, y, seed) triple folded to [0, 1).
float hash01(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
    std::uint32_t h = hashU32(x * 0x9E3779B9U ^ hashU32(y * 0x85EBCA6BU ^ seed));
    return static_cast<float>(h & 0x00FFFFFFU) / 16777216.0f;
}

unsigned char toByte(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<unsigned char>(v * 255.0f + 0.5f);
}

void putPixel(std::vector<unsigned char>& pixels, int size, int x, int y,
              const glm::vec3& c) {
    const size_t i = (static_cast<size_t>(y) * size + x) * 4;
    pixels[i + 0] = toByte(c.r);
    pixels[i + 1] = toByte(c.g);
    pixels[i + 2] = toByte(c.b);
    pixels[i + 3] = 255; // fully opaque — alpha unused in phase 1
}

// Sprite variant: writes an explicit alpha. Only the alpha channel is the
// cutout; the shader discards texels below its alpha-test threshold. We never
// blend, so anything not flatly 0 or 255 just shifts where that hard edge
// lands (used by Stain's soft-ish falloff).
void putPixelA(std::vector<unsigned char>& pixels, int size, int x, int y,
               const glm::vec3& c, float alpha) {
    const size_t i = (static_cast<size_t>(y) * size + x) * 4;
    pixels[i + 0] = toByte(c.r);
    pixels[i + 1] = toByte(c.g);
    pixels[i + 2] = toByte(c.b);
    pixels[i + 3] = toByte(alpha);
}

// Pull 1-2 muted colors out of the palette by hashing the seed. The dream's
// tone is quiet dread, never carnival — so we desaturate hard toward gray and
// cap brightness. Empty palette falls back to a dim neutral gray. colorB is
// derived from a second hash and pushed darker, giving generators a fg/bg or
// detail/base pair without demanding two real palette entries.
void pickColors(const std::vector<glm::vec3>& palette, std::uint32_t seed,
                glm::vec3& colorA, glm::vec3& colorB) {
    const glm::vec3 fallback(0.32f, 0.31f, 0.30f);
    auto mute = [](glm::vec3 c) {
        // Desaturate toward luminance, then clamp the ceiling: no neon.
        const float l = glm::dot(c, glm::vec3(0.299f, 0.587f, 0.114f));
        c = glm::mix(c, glm::vec3(l), 0.55f);
        return glm::min(c, glm::vec3(0.6f));
    };

    if (palette.empty()) {
        colorA = fallback;
        colorB = fallback * 0.5f;
        return;
    }

    const std::uint32_t n = static_cast<std::uint32_t>(palette.size());
    colorA = mute(palette[hashU32(seed) % n]);
    colorB = mute(palette[hashU32(seed ^ 0xA53C9D17U) % n]) * 0.55f;
}

} // namespace

Texture Texture::procedural(Procedural kind, int size,
                            const glm::vec3& colorA, const glm::vec3& colorB,
                            unsigned int seed) {
    // White is always a single texel; with GL_REPEAT + nearest it covers any
    // surface, and the shader's uColor tint does the actual coloring.
    if (kind == Procedural::White) {
        size = 1;
    }
    if (size < 1) {
        size = 1;
    }

    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            glm::vec3 c;
            switch (kind) {
            case Procedural::Grid:
                // colorA fill, 1px colorB lines every 8 texels. Lines land on
                // texel 0 of each cell, so the pattern wraps cleanly under
                // GL_REPEAT (the edge line is shared with the next tile).
                c = (x % 8 == 0 || y % 8 == 0) ? colorB : colorA;
                break;
            case Procedural::Checker:
                // 8x8 texel cells. Parity of the cell coordinates picks the
                // color; size being a multiple of 16 keeps the wrap seamless.
                c = (((x / 8) + (y / 8)) % 2 == 0) ? colorA : colorB;
                break;
            case Procedural::Noise: {
                // Two layers of hash noise: a coarse lattice (every 4 texels)
                // for blotchy structure plus per-texel grain. No smoothing on
                // purpose — interpolated noise looks soft, and soft is the
                // enemy here. Hash is pure function of (x, y, seed) so it
                // tiles... not at all, but at PS1 resolution nobody notices
                // the seam, and the wrongness is on-theme.
                float t = 0.5f * hash01(static_cast<std::uint32_t>(x),
                                        static_cast<std::uint32_t>(y), seed) +
                          0.5f * hash01(static_cast<std::uint32_t>(x / 4),
                                        static_cast<std::uint32_t>(y / 4), seed ^ 0x5bd1e995U);
                c = glm::mix(colorA, colorB, t);
                break;
            }
            case Procedural::White:
                c = glm::vec3(1.0f);
                break;
            default: {
                // v3 material themes: one brightness field per kind, then a
                // 4x4 Bayer dither quantizing to 6 levels — the PS1-texture
                // banding is the point, not an accident.
                const auto ux = static_cast<std::uint32_t>(x);
                const auto uy = static_cast<std::uint32_t>(y);
                float v = 0.6f;
                switch (kind) {
                case Procedural::Concrete:
                    v = 0.62f + 0.18f * hash01(ux / 5, uy / 5, seed) +
                        0.08f * hash01(ux, uy, seed ^ 0x9D2CU);
                    if (hash01(ux, uy, seed ^ 0x51EDU) > 0.97f) v -= 0.30f; // pits
                    break;
                case Procedural::Wood: {
                    const std::uint32_t plank = ux / 16;
                    v = 0.55f + 0.22f * hash01(plank, 0, seed);          // per-plank shade
                    v += 0.10f * hash01(plank, uy / 3, seed ^ 0x77U);    // grain bands
                    if (x % 16 == 0) v -= 0.28f;                          // seams
                    break;
                }
                case Procedural::Metal: {
                    // Corrugation: triangle wave across x, period 16.
                    const int p = x % 16;
                    const float tri = (p < 8 ? p : 16 - p) / 8.0f;
                    v = 0.45f + 0.35f * tri;
                    if (hash01(ux / 2, uy / 2, seed ^ 0xB0BU) > 0.96f) v -= 0.25f; // rust
                    break;
                }
                case Procedural::Brick: {
                    const int row = y / 8;
                    const int xo = (row % 2) ? x + 8 : x;
                    if (y % 8 == 0 || xo % 16 == 0) {
                        v = 0.80f; // mortar
                    } else {
                        v = 0.50f + 0.18f * hash01(static_cast<std::uint32_t>(xo / 16),
                                                   static_cast<std::uint32_t>(row), seed) +
                            0.06f * hash01(ux, uy, seed ^ 0xBEEFU);
                    }
                    break;
                }
                case Procedural::Plaster:
                    v = 0.72f + 0.05f * hash01(ux, uy, seed) -
                        0.12f * hash01(ux / 13, uy / 13, seed ^ 0x57A1U);
                    break;
                case Procedural::Grass:
                    v = 0.50f + 0.25f * hash01(ux, uy / 3, seed) +
                        0.10f * hash01(ux / 7, uy / 7, seed ^ 0x6A55U);
                    break;
                case Procedural::Dirt:
                    v = 0.52f + 0.20f * hash01(ux / 6, uy / 6, seed) +
                        0.12f * hash01(ux, uy, seed ^ 0xD127U);
                    break;
                case Procedural::Water: {
                    const float wob = std::sin(x * 0.40f + hash01(0, uy / 8, seed) * 6.28f);
                    const float band = std::sin((y + wob * 2.0f) * 0.55f);
                    v = 0.55f + 0.20f * band + 0.06f * hash01(ux, uy, seed ^ 0x3EAU);
                    break;
                }
                default: break;
                }
                // Bayer 4x4 ordered dither, 6 brightness levels.
                static const float kBayer[16] = {
                    0,  8,  2, 10,
                    12, 4, 14,  6,
                    3, 11,  1,  9,
                    15, 7, 13,  5};
                const float dith = (kBayer[(y % 4) * 4 + (x % 4)] / 16.0f - 0.5f) / 6.0f;
                v = std::clamp(v + dith, 0.0f, 1.0f);
                v = std::round(v * 5.0f) / 5.0f;
                c = glm::mix(colorB, colorA, v);
                break;
            }
            }
            putPixel(pixels, size, x, y, c);
        }
    }

    // Patterns tile across surfaces, so they repeat.
    return upload(pixels, size, GL_REPEAT);
}

Texture Texture::upload(const std::vector<unsigned char>& pixels, int size,
                        int wrap) {
    return uploadWH(pixels.data(), size, size, wrap);
}

Texture Texture::uploadWH(const unsigned char* pixels, int w, int h,
                          int wrap) {
    Texture tex;
    tex.m_aspect = (h > 0) ? static_cast<float>(w) / static_cast<float>(h)
                           : 1.0f;
    glGenTextures(1, &tex.m_id);
    glBindTexture(GL_TEXTURE_2D, tex.m_id);

    // Sampler state lives on the texture object in plain GL (no separate
    // sampler objects needed at this scale). Set it before upload out of
    // habit — order doesn't matter, but forgetting it entirely does (default
    // min filter is mipmapping, see header comment).
    // Point-sampled mipmaps: GL_NEAREST_MIPMAP_NEAREST keeps the hard-texel
    // (no bilinear) look while picking a mip sized to the on-screen footprint,
    // so minifying a large texture reads coherent texels instead of thrashing
    // the GPU cache. Still nearest, never linear (look invariant intact). Mag
    // has no mip and stays GL_NEAREST.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);

    // RGBA8 internal format, RGBA/unsigned-byte client data. 4 bytes per
    // pixel means every row is naturally 4-byte aligned, so the default
    // GL_UNPACK_ALIGNMENT of 4 is safe even for odd sizes.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Build the mip chain the GL_NEAREST_MIPMAP_NEAREST min filter selects from.
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

std::optional<Texture> Texture::fromFile(const std::string& path) {
    int w = 0, h = 0, channels = 0;
    // Force 4 channels so the upload path stays RGBA8 regardless of source.
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> data(
        stbi_load(path.c_str(), &w, &h, &channels, 4), &stbi_image_free);
    if (!data || w <= 0 || h <= 0) {
        return std::nullopt;
    }
    return uploadWH(data.get(), w, h, GL_CLAMP_TO_EDGE);
}

std::optional<Texture> Texture::fromMemory(const unsigned char* bytes,
                                           std::size_t len) {
    if (!bytes || len == 0) return std::nullopt;
    int w = 0, h = 0, channels = 0;
    // Same RAII guard and forced-RGBA contract as fromFile; only the source
    // differs (memory buffer instead of a path).
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> data(
        stbi_load_from_memory(bytes, static_cast<int>(len), &w, &h, &channels, 4),
        &stbi_image_free);
    if (!data || w <= 0 || h <= 0) {
        return std::nullopt;
    }
    return uploadWH(data.get(), w, h, GL_CLAMP_TO_EDGE);
}

std::optional<Texture> Texture::fromPixels(int w, int h,
                                           const unsigned char* rgba) {
    if (w <= 0 || h <= 0 || !rgba) return std::nullopt;
    return uploadWH(rgba, w, h, GL_CLAMP_TO_EDGE);
}

Texture Texture::sprite(Sprite kind, int size,
                        const std::vector<glm::vec3>& palette,
                        unsigned int seed) {
    if (size < 1) {
        size = 1;
    }

    // Start FULLY TRANSPARENT. Every generator is additive: it paints opaque
    // (or near-opaque) texels onto an empty field. What it never touches stays
    // alpha 0 and gets discarded by the shader's alpha-test, which is how a
    // sprite becomes a cutout without any blending. (procedural() can't do
    // this — it leaves alpha pinned at 255.)
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4, 0);

    glm::vec3 colorA, colorB;
    pickColors(palette, seed, colorA, colorB);

    // Mirror helper: the brain finds faces/figures in symmetric forms far
    // faster than in asymmetric ones (pareidolia leans on bilateral symmetry),
    // so most generators paint the left half and reflect it across the vertical
    // axis. `mx` maps an x in [0, size) to its mirror.
    const int mx_max = size - 1;
    auto stampSym = [&](int x, int y, const glm::vec3& c, float a) {
        if (x < 0 || x >= size || y < 0 || y >= size) return;
        putPixelA(pixels, size, x, y, c, a);
        putPixelA(pixels, size, mx_max - x, y, c, a);
    };
    auto stamp = [&](int x, int y, const glm::vec3& c, float a) {
        if (x < 0 || x >= size || y < 0 || y >= size) return;
        putPixelA(pixels, size, x, y, c, a);
    };

    const float fsize = static_cast<float>(size);

    switch (kind) {
    case Sprite::Face: {
        // A face is the strongest pareidolia trigger we have. Recipe: a faint
        // noisy "skin" field that's mostly opaque so it reads as a solid head,
        // then two dark sunken eye sockets and a mouth gouged into it, all
        // mirrored. We don't draw a head outline — the noisy fill + the dark
        // features are enough for the eye to complete the face, and the ragged
        // alpha edge keeps it from looking like a clean icon.
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float n = hash01(static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), seed);
                // Skin: colorA dimmed and grained. Mostly opaque, but punch a
                // few transparent holes via the low tail of the noise so the
                // silhouette is eaten at random — a clean rectangle reads as a
                // texture swatch, a holey one reads as a thing.
                const glm::vec3 skin = colorA * (0.30f + 0.25f * n);
                const float a = (n < 0.06f) ? 0.0f : 1.0f;
                stamp(x, y, skin, a);
            }
        }
        // Eye sockets: two dark circles in the upper third, set off-center so
        // they sit where eyes belong. Mirrored, so spacing is automatic.
        const int eyeY = size * 38 / 100;
        const int eyeX = size * 30 / 100; // distance from center (left eye)
        const int eyeR = std::max(1, size / 10);
        const int cx = size / 2;
        for (int dy = -eyeR; dy <= eyeR; ++dy) {
            for (int dx = -eyeR; dx <= eyeR; ++dx) {
                if (dx * dx + dy * dy > eyeR * eyeR) continue;
                // colorB is the darkest pair member — sockets read as voids.
                stamp(cx - eyeX + dx, eyeY + dy, colorB * 0.3f, 1.0f);
            }
        }
        // Mouth: a dark horizontal gash low and centered, slightly noisy at the
        // ends so it doesn't look drawn-on. Symmetric about the center column.
        const int mouthY = size * 70 / 100;
        const int mouthHalf = size * 22 / 100;
        const int mouthThick = std::max(1, size / 16);
        for (int x = 0; x <= mouthHalf; ++x) {
            const float n = hash01(static_cast<std::uint32_t>(x), 7u, seed);
            const int thick = mouthThick + (n > 0.7f ? 1 : 0);
            for (int t = 0; t < thick; ++t) {
                stampSym(cx - x, mouthY + t, colorB * 0.35f, 1.0f);
            }
        }
        break;
    }
    case Sprite::Sigil: {
        // A scratched occult mark. We build a tiny stroke graph: pick a handful
        // of node points on a coarse grid and connect random pairs with
        // straight runs of opaque texels. Mirroring the whole thing across the
        // center gives it the deliberate, ritual symmetry of a real sigil
        // without us having to design one. Field stays transparent.
        const int strokes = 3 + static_cast<int>(hashU32(seed) % 3); // 3..5
        const int margin = size / 6;
        for (int s = 0; s < strokes; ++s) {
            const std::uint32_t hs = hashU32(seed + static_cast<std::uint32_t>(s) * 0x68E31DA4U);
            // Endpoints snapped to a coarse lattice so strokes meet cleanly.
            const int gx0 = margin + static_cast<int>(hs % 5) * (size - 2 * margin) / 4;
            const int gy0 = margin + static_cast<int>((hs >> 3) % 5) * (size - 2 * margin) / 4;
            const int gx1 = margin + static_cast<int>((hs >> 6) % 5) * (size - 2 * margin) / 4;
            const int gy1 = margin + static_cast<int>((hs >> 9) % 5) * (size - 2 * margin) / 4;
            // Integer DDA line — keeps every texel on the run filled, no gaps.
            const int dx = std::abs(gx1 - gx0), dy = std::abs(gy1 - gy0);
            const int n = std::max(dx, dy);
            for (int i = 0; i <= n; ++i) {
                const float t = (n == 0) ? 0.0f : static_cast<float>(i) / n;
                const int px = gx0 + static_cast<int>((gx1 - gx0) * t + 0.5f);
                const int py = gy0 + static_cast<int>((gy1 - gy0) * t + 0.5f);
                // Mirror across vertical axis for sigil-like balance.
                stampSym(px, py, colorA, 1.0f);
            }
        }
        break;
    }
    case Sprite::Stain: {
        // A smear bled into the wall. Directional noise: we stretch the noise
        // lattice along one axis (chosen from the seed) so the blotch has a run
        // direction, like something dripped or was wiped. Alpha is the noise
        // value falling off with distance from center — but since we can't
        // blend, "falloff" really means a soft alpha threshold: texels above it
        // are opaque, below it vanish, so the edge frays instead of fading.
        const bool vertical = (hashU32(seed) & 1u) != 0u;
        const float cx = fsize * 0.5f, cy = fsize * 0.5f;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                // Stretch the sample coords so the lattice is anisotropic.
                const std::uint32_t sx = static_cast<std::uint32_t>(vertical ? x / 2 : x);
                const std::uint32_t sy = static_cast<std::uint32_t>(vertical ? y : y / 2);
                const float n = 0.6f * hash01(sx, sy, seed) +
                                0.4f * hash01(sx / 3, sy / 3, seed ^ 0x27D4EB2FU);
                // Radial falloff (1 at center -> 0 at the corners).
                const float dxn = (x - cx) / cx, dyn = (y - cy) / cy;
                const float r = std::sqrt(dxn * dxn + dyn * dyn);
                const float density = n * (1.0f - std::min(1.0f, r * 0.9f));
                // Hard threshold = the only "blending" we get with GL_BLEND off.
                if (density > 0.35f) {
                    const glm::vec3 c = glm::mix(colorB, colorA, n);
                    stamp(x, y, c, 1.0f);
                }
            }
        }
        break;
    }
    case Sprite::Figure:
    case Sprite::FigureEyes: {
        // A standing person seen at distance — billboard silhouette against
        // transparency. What makes a dark shape read "human" rather than
        // "post" is anatomy in negative space: a neck pinch, sloped shoulders,
        // arms hanging SEPARATE from the torso (the gap of light between arm
        // and body is the tell), two legs with daylight between them. Built
        // asymmetric on purpose (no stampSym): one shoulder a little low, one
        // leg mid-step, head leaned — a pose, not an icon. Same seed gives the
        // same pose, so Figure and FigureEyes line up texel-for-texel and the
        // eyes can be swapped in at close range without the body shifting.
        const std::uint32_t hp = hashU32(seed ^ 0x51ED270BU);
        // Asymmetry unit scales with resolution so a 96px sprite slouches as
        // visibly as a 64px one.
        const int unit = std::max(1, size / 48);
        const int cx = size / 2;
        auto pct = [&](int p) { return size * p / 100; };

        // Body plan, top to bottom (row y=0 is the sprite's top).
        const int headTop = pct(8), headBot = pct(22);
        const int neckBot = pct(26);
        const int shoulderBot = pct(32);
        const int waist = pct(55);
        const int feet = pct(96);
        const int headHalf = std::max(2, pct(6));
        const int neckHalf = std::max(1, pct(2));
        const int shoulderHalf = std::max(3, pct(10));
        const int waistHalf = std::max(2, pct(6));
        const int legW = std::max(2, pct(3));      // each leg's width
        const int legGap = std::max(1, pct(2));    // half the gap between legs

        // The pose, rolled once from the seed.
        const int tilt = (static_cast<int>(hp % 3) - 1) * unit;        // head lean
        const int shoulderDrop = (1 + static_cast<int>((hp >> 2) % 2)) * unit;
        const bool dropLeft = ((hp >> 4) & 1u) != 0u;
        const int armEndL = pct(58 + static_cast<int>((hp >> 6) % 8));
        const int armEndR = pct(58 + static_cast<int>((hp >> 9) % 8));
        const int legStep = static_cast<int>((hp >> 12) % 3) * unit;   // one foot short
        const bool stepLeft = ((hp >> 14) & 1u) != 0u;

        // Fill a horizontal body span, fraying the outermost texels on noise so
        // the outline shivers instead of reading as a clean cutout.
        auto span = [&](int x0, int x1, int y, float frayP) {
            for (int x = x0; x <= x1; ++x) {
                const bool edge = (x == x0) || (x == x1);
                const float n = hash01(static_cast<std::uint32_t>(x + 128),
                                       static_cast<std::uint32_t>(y), seed);
                if (edge && n < frayP) continue;
                stamp(x, y, colorB * (0.25f + 0.15f * n), 1.0f);
            }
        };

        for (int y = headTop; y <= feet; ++y) {
            if (y <= headBot) {
                // Head: an ellipse, leaned off the spine by `tilt`.
                const float t = (y - (headTop + headBot) * 0.5f) /
                                ((headBot - headTop) * 0.5f);
                const int hw = std::max(1, static_cast<int>(
                    headHalf * std::sqrt(std::max(0.0f, 1.0f - t * t))));
                span(cx + tilt - hw, cx + tilt + hw, y, 0.4f);
            } else if (y <= neckBot) {
                // Neck: the pinch. Without it the head fuses into the torso
                // and the whole thing reads as a bottle.
                span(cx - neckHalf, cx + neckHalf, y, 0.4f);
            } else if (y <= shoulderBot) {
                // Shoulders: each side ramps from neck width out to full
                // shoulder width, the dropped side starting its ramp late so
                // it sits visibly lower.
                auto ramp = [&](int startY) {
                    const float t = glm::clamp(
                        (y - startY) / float(std::max(1, shoulderBot - neckBot)),
                        0.0f, 1.0f);
                    return neckHalf + static_cast<int>((shoulderHalf - neckHalf) * t);
                };
                const int lh = ramp(neckBot + (dropLeft ? shoulderDrop : 0));
                const int rh = ramp(neckBot + (dropLeft ? 0 : shoulderDrop));
                span(cx - lh, cx + rh, y, 0.4f);
            } else if (y <= waist) {
                // Torso: taper from shoulders to waist.
                const float t = (y - shoulderBot) / float(std::max(1, waist - shoulderBot));
                const int hw = shoulderHalf + static_cast<int>((waistHalf - shoulderHalf) * t);
                span(cx - hw, cx + hw, y, 0.4f);
            } else {
                // Legs: two columns with a transparent gap between them; one
                // foot ends a touch short (mid-step).
                const int footL = feet - (stepLeft ? legStep : 0);
                const int footR = feet - (stepLeft ? 0 : legStep);
                if (y <= footL) span(cx - legGap - legW, cx - legGap, y, 0.4f);
                if (y <= footR) span(cx + legGap, cx + legGap + legW, y, 0.4f);
            }
        }

        // Arms: thin columns hanging OUTSIDE the torso with a gap of clear
        // alpha between arm and body — the single strongest "person" cue this
        // silhouette has. Each drifts outward a hair as it falls and ends at
        // its own height.
        const int armGap = std::max(1, pct(2));
        const int armTopL = neckBot + (dropLeft ? shoulderDrop : 0) + unit;
        const int armTopR = neckBot + (dropLeft ? 0 : shoulderDrop) + unit;
        auto arm = [&](int top, int end, int side) {
            for (int y = top; y <= end; ++y) {
                const int drift = (y - top) / std::max(1, size / 6); // slow lean out
                const int ax = cx + side * (shoulderHalf + armGap + drift);
                const float n = hash01(static_cast<std::uint32_t>(ax + 512),
                                       static_cast<std::uint32_t>(y), seed);
                if (y > end - pct(4) && n < 0.4f) continue; // hands trail off
                span(ax, ax + (unit > 1 ? unit - 1 : 0), y, 0.25f);
            }
        };
        arm(armTopL, armEndL, -1);
        arm(armTopR, armEndR, +1);

        if (kind == Sprite::FigureEyes) {
            // Two pale points, dead level, in the head — the same shine as the
            // Eyes decal. Only the close-range texture carries them.
            const int eyeY = pct(14);
            const int eyeDx = std::max(1, headHalf / 2);
            const int eyeR = std::max(1, unit);
            for (int side = -1; side <= 1; side += 2) {
                const int ex = cx + tilt + side * eyeDx;
                for (int dy = -eyeR; dy <= eyeR; ++dy) {
                    for (int dx = -eyeR; dx <= eyeR; ++dx) {
                        if (dx * dx + dy * dy > eyeR * eyeR) continue;
                        stamp(ex + dx, eyeY + dy, colorA * 0.9f + glm::vec3(0.25f), 1.0f);
                    }
                }
            }
        }
        break;
    }
    case Sprite::Scrawl: {
        // Wall writing in a language that isn't one. A few rows of short
        // horizontal strokes of varying length and gap — the rhythm of text
        // without legible glyphs, which is more unsettling than any real word.
        // Transparent field; strokes in a palette color. Not mirrored: writing
        // shouldn't be symmetric, that would break the illusion of language.
        const int rows = 3 + static_cast<int>(hashU32(seed ^ 0x1B56C4E9U) % 3); // 3..5
        const int rowH = size / (rows + 1);
        const int strokeThick = std::max(1, size / 24);
        for (int r = 0; r < rows; ++r) {
            const int baseY = rowH * (r + 1);
            int x = size / 12; // left margin, jittered per row below
            x += static_cast<int>(hashU32(seed + static_cast<std::uint32_t>(r)) % 4);
            while (x < size - size / 12) {
                const std::uint32_t h = hashU32(seed * 2654435761U +
                                                static_cast<std::uint32_t>(r) * 40503u +
                                                static_cast<std::uint32_t>(x));
                const int runLen = 2 + static_cast<int>(h % (size / 8 + 1)); // glyph-ish run
                const int gap = 2 + static_cast<int>((h >> 5) % 4);          // inter-glyph space
                const int yJit = static_cast<int>((h >> 9) % 2);             // baseline wobble
                for (int i = 0; i < runLen && x + i < size; ++i) {
                    for (int t = 0; t < strokeThick; ++t) {
                        stamp(x + i, baseY + yJit + t, colorA, 1.0f);
                    }
                }
                x += runLen + gap;
            }
        }
        break;
    }
    case Sprite::Handprint: {
        // Someone touched this. A palm blob low-center with five finger
        // strokes splayed upward. NOT mirrored — a hand is the one shape here
        // that must be asymmetric to read as real; the seed skews the splay
        // so no two prints sit at the same angle. Edges fray on noise like a
        // smeared press, not a clean stencil.
        const int cx = size / 2;
        const int palmCy = size * 62 / 100;
        const int palmR = size * 16 / 100;
        for (int dy = -palmR; dy <= palmR; ++dy) {
            for (int dx = -palmR; dx <= palmR; ++dx) {
                // Slightly taller than wide; palms are.
                const float rr = (dx * dx) / float(palmR * palmR) +
                                 (dy * dy) / float(palmR * palmR * 1.4f);
                if (rr > 1.0f) continue;
                const float n = hash01(static_cast<std::uint32_t>(dx + 64),
                                       static_cast<std::uint32_t>(dy + 64), seed);
                if (rr > 0.7f && n < 0.45f) continue; // frayed rim
                stamp(cx + dx, palmCy + dy, glm::mix(colorB, colorA, n) * 0.6f, 1.0f);
            }
        }
        // Five fingers fanned from the palm top. Base angles spread over
        // ~70deg; the seed leans the whole fan and jitters each digit.
        const float lean = (hash01(3u, 9u, seed) - 0.5f) * 0.6f; // radians
        for (int f = 0; f < 5; ++f) {
            const float jit = (hash01(static_cast<std::uint32_t>(f), 11u, seed) - 0.5f) * 0.25f;
            const float a = -1.20f + 0.60f * f + lean + jit; // fan, ~vertical
            // Thumb (f==0) short and wide-set; middle finger longest.
            const float len = fsize * (f == 0 ? 0.18f : 0.26f + 0.04f * (2 - std::abs(f - 2)));
            const int baseX = cx + static_cast<int>((f - 2) * palmR * 0.45f);
            const int baseY = palmCy - palmR;
            const int n = static_cast<int>(len);
            for (int i = 0; i < n; ++i) {
                const int px = baseX + static_cast<int>(std::sin(a) * i);
                const int py = baseY - static_cast<int>(std::cos(a) * i);
                const float fn = hash01(static_cast<std::uint32_t>(f * 97 + i), 13u, seed);
                if (i > n * 3 / 4 && fn < 0.35f) continue; // tips trail off
                stamp(px, py, glm::mix(colorB, colorA, fn) * 0.6f, 1.0f);
                if (i < n / 2) stamp(px + 1, py, colorB * 0.55f, 1.0f); // thicker base
            }
        }
        break;
    }
    case Sprite::Eyes: {
        // Eyes and nothing else: pale points set in dark sockets on an empty
        // field, the way eyeshine reads from a doorway. Floated mid-air or on
        // a wall, the brain supplies the body. 1-3 pairs, each placed and
        // sized off the seed; pairs are level (real eyes are), which is what
        // separates "watcher" from "random dots".
        const int pairs = 1 + static_cast<int>(hashU32(seed ^ 0x9E3779B9U) % 3);
        for (int p = 0; p < pairs; ++p) {
            const std::uint32_t hp = hashU32(seed + static_cast<std::uint32_t>(p) * 0x85EBCA6BU);
            const int eyeR = std::max(1, size / 16 + static_cast<int>(hp % (size / 24 + 1)));
            const int gap = eyeR * (3 + static_cast<int>((hp >> 4) % 2)); // center-to-center
            const int cxp = size / 4 + static_cast<int>((hp >> 8) % (size / 2));
            const int cyp = size / 4 + static_cast<int>((hp >> 16) % (size / 2));
            for (int side = -1; side <= 1; side += 2) {
                const int ex = cxp + side * gap / 2;
                // Socket: a void twice the iris, eaten at the rim.
                const int sockR = eyeR * 2;
                for (int dy = -sockR; dy <= sockR; ++dy) {
                    for (int dx = -sockR; dx <= sockR; ++dx) {
                        if (dx * dx + dy * dy > sockR * sockR) continue;
                        const float n = hash01(static_cast<std::uint32_t>(ex + dx),
                                               static_cast<std::uint32_t>(cyp + dy), seed);
                        if (dx * dx + dy * dy > sockR * sockR * 3 / 4 && n < 0.5f) continue;
                        stamp(ex + dx, cyp + dy, colorB * 0.18f, 1.0f);
                    }
                }
                // The shine: small, bright, dead level with its twin.
                for (int dy = -eyeR; dy <= eyeR; ++dy) {
                    for (int dx = -eyeR; dx <= eyeR; ++dx) {
                        if (dx * dx + dy * dy > eyeR * eyeR) continue;
                        stamp(ex + dx, cyp + dy, colorA * 0.9f + glm::vec3(0.25f), 1.0f);
                    }
                }
            }
        }
        break;
    }
    case Sprite::Hanging: {
        // A line from the top edge ending in a slumped mass — rope and weight,
        // never named. Row y=0 is the sprite's top (see Figure: head at low
        // y), so the line starts at y=0 and the mass hangs below it. Slight
        // x-drift on the line so it reads as hanging under tension, not drawn
        // with a ruler.
        const std::uint32_t h = hashU32(seed ^ 0x2545F491U);
        const int cx = size * 35 / 100 + static_cast<int>(h % (size * 30 / 100 + 1));
        const int ropeEnd = size * (35 + static_cast<int>((h >> 8) % 20)) / 100;
        int x = cx;
        for (int y = 0; y <= ropeEnd; ++y) {
            const float n = hash01(5u, static_cast<std::uint32_t>(y), seed);
            if (n > 0.92f) x += (n > 0.96f) ? 1 : -1; // rare kink
            stamp(x, y, colorB * 0.30f, 1.0f);
            if (size >= 48) stamp(x + 1, y, colorB * 0.30f, 1.0f);
        }
        // The mass: a sagging ellipse under the rope end, wider than tall at
        // the shoulders then tapering — slumped, not spherical. Frayed rim.
        const int massH = size * 30 / 100;
        const int massW = size * 11 / 100;
        for (int dy = 0; dy <= massH; ++dy) {
            const float t = dy / float(massH);
            // Widest a third down, pinched at top (where the rope bites) and
            // trailing at the bottom.
            const float w = massW * (0.35f + 1.5f * t * (1.0f - t) * 2.0f * (1.0f - 0.4f * t));
            const int half = std::max(1, static_cast<int>(w));
            for (int dx = -half; dx <= half; ++dx) {
                const float n = hash01(static_cast<std::uint32_t>(dx + 32),
                                       static_cast<std::uint32_t>(dy + 200), seed);
                if (std::abs(dx) >= half - 1 && n < 0.4f) continue;
                stamp(x + dx, ropeEnd + dy, colorB * (0.22f + 0.12f * n), 1.0f);
            }
        }
        break;
    }
    case Sprite::Drip: {
        // Something bled through from above: an opaque blotch hugging the top
        // edge with a few tapering runs crawling down, each its own length —
        // the Stain's threshold trick turned vertical so gravity shows.
        const int blotchH = size * 18 / 100;
        for (int y = 0; y < blotchH; ++y) {
            for (int x = 0; x < size; ++x) {
                const float n = 0.6f * hash01(static_cast<std::uint32_t>(x),
                                              static_cast<std::uint32_t>(y), seed) +
                                0.4f * hash01(static_cast<std::uint32_t>(x / 3),
                                              static_cast<std::uint32_t>(y / 3),
                                              seed ^ 0x27D4EB2FU);
                // Density fades toward the blotch's lower lip so the underside
                // frays into the runs instead of ending on a shelf.
                const float density = n * (1.0f - 0.8f * y / float(blotchH));
                if (density > 0.18f) {
                    stamp(x, y, glm::mix(colorB, colorA, n) * 0.6f, 1.0f);
                }
            }
        }
        // Runs: 2-4 of them, length and width hashed per run, thinning as
        // they fall and dying on noise so no two ends look alike.
        const int runs = 2 + static_cast<int>(hashU32(seed ^ 0x6C8E9CF5U) % 3);
        for (int r = 0; r < runs; ++r) {
            const std::uint32_t hr = hashU32(seed + static_cast<std::uint32_t>(r) * 0xC2B2AE35U);
            const int rx = size / 8 + static_cast<int>(hr % (size * 3 / 4));
            const int rlen = size * (25 + static_cast<int>((hr >> 6) % 60)) / 100;
            for (int y = blotchH; y < blotchH + rlen && y < size; ++y) {
                const float t = (y - blotchH) / float(rlen);
                const int half = std::max(0, static_cast<int>((1.0f - t) * size / 32.0f));
                const float n = hash01(static_cast<std::uint32_t>(r * 31),
                                       static_cast<std::uint32_t>(y), seed);
                if (t > 0.6f && n < t - 0.5f) break; // run starves and stops
                for (int dx = -half; dx <= half; ++dx) {
                    stamp(rx + dx, y, glm::mix(colorB, colorA, n) * 0.55f, 1.0f);
                }
            }
        }
        break;
    }
    }

    // A sprite is a single decal/billboard, never tiled — clamp so sampling
    // outside [0,1] grabs the transparent border instead of wrapping the
    // figure's far edge back into view.
    return upload(pixels, size, GL_CLAMP_TO_EDGE);
}

Texture::~Texture() {
    if (m_id != 0) {
        glDeleteTextures(1, &m_id);
    }
}

Texture::Texture(Texture&& other) noexcept
    : m_id(other.m_id), m_aspect(other.m_aspect) {
    other.m_id = 0; // source must not delete the GL object we now own
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (m_id != 0) {
            glDeleteTextures(1, &m_id);
        }
        m_id = other.m_id;
        m_aspect = other.m_aspect;
        other.m_id = 0;
    }
    return *this;
}

void Texture::bind(int unit) const {
    // Texture units are global GL state: glActiveTexture selects which unit
    // subsequent binds affect. The shader's sampler uniform is set to the
    // same unit index elsewhere; the two only meet through that integer.
    glActiveTexture(GL_TEXTURE0 + static_cast<unsigned int>(unit));
    glBindTexture(GL_TEXTURE_2D, m_id);
}

} // namespace liminal
