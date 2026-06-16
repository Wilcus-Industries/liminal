// Renderer.cpp — the two-pass PS1 pipeline.
//
// Pass 1: scene -> low-res offscreen FBO (~320x240) with the dream shader.
// Pass 2: FBO color texture -> default framebuffer via a fullscreen triangle,
//         nearest-neighbor, letterboxed to preserve the virtual aspect.
//
// Rendering at the *actual* low resolution (rather than faking pixelation in
// a shader) is what makes the look honest: depth testing, fog, vertex
// snapping and texture sampling all happen at 320x240, so every artifact is
// consistent with every other artifact, the way it was on real hardware.

#include <liminal/render/renderer.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

#include <cmath>
#include <vector>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <liminal/render/font8x8.hpp>

// Configure-time embed of assets/shaders/retro/* (see CMakeLists). Gives the
// default-constructed Renderer its shaders with zero filesystem access.
#include "embedded_shaders.hpp"

namespace liminal {

namespace {

std::string slurp(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        throw std::runtime_error("ShaderPack: cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // namespace

ShaderPack ShaderPack::retro() {
    ShaderPack pack;
    pack.sceneVert = embedded::kRetroSceneVert;
    pack.sceneFrag = embedded::kRetroSceneFrag;
    pack.blitVert = embedded::kRetroBlitVert;
    pack.blitFrag = embedded::kRetroBlitFrag;
    pack.label = "retro (embedded)";
    pack.res = ShaderResolution::LowRes;
    return pack;
}

ShaderPack ShaderPack::standard() {
    ShaderPack pack;
    pack.sceneVert = embedded::kNativeSceneVert;
    pack.sceneFrag = embedded::kNativeSceneFrag;
    // The blit pass is shared by both packs — only the scene pass differs.
    pack.blitVert = embedded::kRetroBlitVert;
    pack.blitFrag = embedded::kRetroBlitFrag;
    pack.label = "native (embedded)";
    pack.res = ShaderResolution::Native;
    return pack;
}

namespace {

// Prepended to a frag-only file's body. Frag-only files are BODIES ONLY — no
// #version, no in/out/uniform decls — the engine provides them here. Authors
// write `void main(){ ... }` using these ins/uniforms and write FragColor.
// Must mirror the interface of assets/shaders/native/scene.frag and the
// varyings emitted by embedded::kNativeSceneVert.
constexpr char kFragOnlyHeader[] = R"GLSL(#version 410 core
in vec3 vNormal;
in float vViewDist;
in float vGradT;
smooth in vec2 vUV;

uniform sampler2D uTex;
uniform vec3 uColor;
uniform vec3 uColor2;
uniform vec3 uLightDir;
uniform float uAlphaTest;

out vec4 FragColor;

)GLSL";

} // namespace

ShaderPack ShaderPack::makeFullPack(const std::string& sceneVert,
                                    const std::string& sceneFrag) {
    ShaderPack pack = ShaderPack::standard(); // borrow the shared blit stage
    pack.sceneVert = sceneVert;
    pack.sceneFrag = sceneFrag;
    pack.label = "custom";
    pack.res = ShaderResolution::Native;
    return pack;
}

ShaderPack ShaderPack::makeFragOnlyPack(const std::string& fragBody) {
    ShaderPack pack = ShaderPack::standard(); // borrow the shared blit stage
    // The native vertex stage emits exactly the varyings kFragOnlyHeader
    // declares, so wrapped fragments link cleanly against it.
    pack.sceneVert = embedded::kNativeSceneVert;
    pack.sceneFrag = std::string(kFragOnlyHeader) + fragBody;
    pack.label = "custom (frag-only)";
    pack.res = ShaderResolution::Native;
    return pack;
}

ShaderPack ShaderPack::fromFiles(const std::string& sceneVertPath,
                                 const std::string& sceneFragPath,
                                 const std::string& blitVertPath,
                                 const std::string& blitFragPath) {
    ShaderPack pack;
    pack.sceneVert = slurp(sceneVertPath);
    pack.sceneFrag = slurp(sceneFragPath);
    pack.blitVert = slurp(blitVertPath);
    pack.blitFrag = slurp(blitFragPath);
    pack.label = sceneVertPath; // the scene shader names the pack in errors
    return pack;
}

namespace {

// Compiles a pack's two stages into a CompiledShader. Throws std::runtime_error
// on compile/link failure (propagates the offending stage's message).
std::unique_ptr<Renderer::CompiledShader> compilePack(const ShaderPack& pack) {
    Shader scene = Shader::fromSource(pack.sceneVert, pack.sceneFrag,
                                      pack.label + " [scene]");
    Shader blit = Shader::fromSource(pack.blitVert, pack.blitFrag,
                                     pack.label + " [blit]");
    return std::make_unique<Renderer::CompiledShader>(
        Renderer::CompiledShader{std::move(scene), std::move(blit), pack.res});
}

} // namespace

// Shared GL setup for both ctors: the FBO targets + the fullscreen blit
// triangle, with the partial-construction cleanup the dtor can't run.
void Renderer::buildPipeline() {
    // m_active must already be valid before any draw; initialize the render
    // size from the virtual resolution so createTargets has a size to use.
    m_renderW = settings.virtualW;
    m_renderH = settings.virtualH;
    try {
    createTargets();

    // Fullscreen triangle for the blit pass. One triangle, not a quad: the
    // verts sit at (-1,-1), (3,-1), (-1,3) so the triangle overhangs the
    // screen and the GPU clips it; this avoids the diagonal seam (and the
    // double-shaded pixels along it) that a two-triangle quad produces.
    // UVs are scaled to match (0..2) so the visible region maps to 0..1.
    const float verts[] = {
        // pos.x  pos.y   u     v
        -1.0f, -1.0f, 0.0f, 0.0f,
         3.0f, -1.0f, 2.0f, 0.0f,
        -1.0f,  3.0f, 0.0f, 2.0f,
    };

    glGenVertexArrays(1, &m_fsVao);
    glGenBuffers(1, &m_fsVbo);
    glBindVertexArray(m_fsVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_fsVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    // Interleaved pos2 | uv2, stride 4 floats. Attribute locations match
    // blit.vert's layout qualifiers.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // 2D UI resources (font atlas, white texel, program, batch VAO/VBO).
    buildUiResources();
    } catch (...) {
        // Partially-constructed object: ~Renderer won't run, so free any GL
        // objects created so far before rethrowing.
        if (m_fsVao) {
            glDeleteVertexArrays(1, &m_fsVao);
            m_fsVao = 0;
        }
        if (m_fsVbo) {
            glDeleteBuffers(1, &m_fsVbo);
            m_fsVbo = 0;
        }
        if (m_uiVao) {
            glDeleteVertexArrays(1, &m_uiVao);
            m_uiVao = 0;
        }
        if (m_uiVbo) {
            glDeleteBuffers(1, &m_uiVbo);
            m_uiVbo = 0;
        }
        if (m_fontTex) {
            glDeleteTextures(1, &m_fontTex);
            m_fontTex = 0;
        }
        if (m_whiteTex2d) {
            glDeleteTextures(1, &m_whiteTex2d);
            m_whiteTex2d = 0;
        }
        m_uiShader.reset();
        destroyTargets();
        throw;
    }
}

// One-time GL setup for the immediate-mode 2D UI pass: bakes the 8x8 font atlas
// + a 1x1 white texel, compiles the textured-color 2D program, and allocates a
// dynamic VAO/VBO the per-frame batches stream into (uploaded in flushUi).
void Renderer::buildUiResources() {
    // --- Font atlas: glyphs 0x20..0x7E into a 16-column grid (95 glyphs). ---
    constexpr int kCols = 16;
    constexpr int kGlyphs = 95; // 0x20..0x7E
    constexpr int kRows = (kGlyphs + kCols - 1) / kCols; // 6
    m_fontAtlasW = kCols * 8;  // 128
    m_fontAtlasH = kRows * 8;  // 48
    std::vector<unsigned char> atlas(
        static_cast<std::size_t>(m_fontAtlasW) * m_fontAtlasH * 4, 0);
    for (int i = 0; i < kGlyphs; ++i) {
        const int c = 0x20 + i;
        const int col = i % kCols;
        const int row = i / kCols;
        for (int ry = 0; ry < 8; ++ry) {
            const unsigned char bits = kFont8x8Basic[c][ry];
            for (int rx = 0; rx < 8; ++rx) {
                if (!((bits >> rx) & 1)) continue;
                const int px = col * 8 + rx;
                const int py = row * 8 + ry;
                const std::size_t idx =
                    (static_cast<std::size_t>(py) * m_fontAtlasW + px) * 4;
                atlas[idx + 0] = 255;
                atlas[idx + 1] = 255;
                atlas[idx + 2] = 255;
                atlas[idx + 3] = 255;
            }
        }
    }
    glGenTextures(1, &m_fontTex);
    glBindTexture(GL_TEXTURE_2D, m_fontTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_fontAtlasW, m_fontAtlasH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // --- 1x1 white texel for solid rects/lines (FragColor = vColor). ---
    const unsigned char white[4] = {255, 255, 255, 255};
    glGenTextures(1, &m_whiteTex2d);
    glBindTexture(GL_TEXTURE_2D, m_whiteTex2d);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // --- 2D program: ortho-projected textured vertex color. ---
    static const char* kUiVert = R"GLSL(#version 410 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform mat4 uProj;
out vec2 vUV; out vec4 vColor;
void main(){ vUV=aUV; vColor=aColor; gl_Position=uProj*vec4(aPos,0.0,1.0); }
)GLSL";
    static const char* kUiFrag = R"GLSL(#version 410 core
in vec2 vUV; in vec4 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main(){ FragColor = vColor * texture(uTex, vUV); }
)GLSL";
    m_uiShader = std::make_unique<Shader>(
        Shader::fromSource(kUiVert, kUiFrag, "ui2d"));

    // --- Dynamic batch VAO/VBO: interleaved pos2 | uv2 | color4 (8 floats). ---
    glGenVertexArrays(1, &m_uiVao);
    glGenBuffers(1, &m_uiVbo);
    glBindVertexArray(m_uiVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_uiVbo);
    const GLsizei stride = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                          (const void*)(4 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// Two triangles (6 verts) for a quad in screen space with the given UVs.
void Renderer::pushQuad(std::vector<UiVert>& batch, float x0, float y0,
                        float x1, float y1, float u0, float v0, float u1,
                        float v1, const glm::vec4& c) {
    const UiVert tl{x0, y0, u0, v0, c.r, c.g, c.b, c.a};
    const UiVert bl{x0, y1, u0, v1, c.r, c.g, c.b, c.a};
    const UiVert br{x1, y1, u1, v1, c.r, c.g, c.b, c.a};
    const UiVert tr{x1, y0, u1, v0, c.r, c.g, c.b, c.a};
    batch.push_back(tl);
    batch.push_back(bl);
    batch.push_back(br);
    batch.push_back(tl);
    batch.push_back(br);
    batch.push_back(tr);
}

void Renderer::uiRect(float x, float y, float w, float h,
                      const glm::vec4& color) {
    pushQuad(m_uiSolid, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 0.0f, color);
}

void Renderer::uiText(float x, float y, const std::string& text,
                      const glm::vec4& color, float scale) {
    const float adv = 8.0f * scale;
    const float fw = (float)m_fontAtlasW;
    const float fh = (float)m_fontAtlasH;
    float cursor = x;
    for (unsigned char c : text) {
        if (c >= 0x20 && c <= 0x7E) {
            const int gi = c - 0x20;
            const int col = gi % 16;
            const int row = gi / 16;
            const float u0 = (col * 8) / fw;
            const float v0 = (row * 8) / fh;
            const float u1 = (col * 8 + 8) / fw;
            const float v1 = (row * 8 + 8) / fh;
            pushQuad(m_uiText, cursor, y, cursor + adv, y + adv, u0, v0, u1, v1,
                     color);
        }
        cursor += adv; // space + out-of-range chars advance without drawing
    }
}

void Renderer::uiLine(float x0, float y0, float x1, float y1,
                      const glm::vec4& color, float thickness) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) return; // degenerate
    const float nx = dx / len;
    const float ny = dy / len;
    // Perpendicular, scaled to half the thickness.
    const float px = -ny * thickness * 0.5f;
    const float py = nx * thickness * 0.5f;
    // Four corners → two triangles. uv = 0 (white texel).
    const glm::vec4& c = color;
    const UiVert a{x0 + px, y0 + py, 0, 0, c.r, c.g, c.b, c.a};
    const UiVert b{x0 - px, y0 - py, 0, 0, c.r, c.g, c.b, c.a};
    const UiVert d{x1 - px, y1 - py, 0, 0, c.r, c.g, c.b, c.a};
    const UiVert e{x1 + px, y1 + py, 0, 0, c.r, c.g, c.b, c.a};
    m_uiSolid.push_back(a);
    m_uiSolid.push_back(b);
    m_uiSolid.push_back(d);
    m_uiSolid.push_back(a);
    m_uiSolid.push_back(d);
    m_uiSolid.push_back(e);
}

void Renderer::flushUi() {
    if (m_uiSolid.empty() && m_uiText.empty()) return;
    glViewport(0, 0, m_renderW, m_renderH);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_uiShader->use();
    // Ortho, origin top-left: x 0..W, y 0..H (y down).
    const glm::mat4 proj = glm::ortho(0.0f, (float)m_renderW,
                                      (float)m_renderH, 0.0f, -1.0f, 1.0f);
    m_uiShader->set("uProj", proj);
    m_uiShader->set("uTex", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m_uiVao);
    auto drawBatch = [&](std::vector<UiVert>& b, unsigned int tex) {
        if (b.empty()) return;
        glBindBuffer(GL_ARRAY_BUFFER, m_uiVbo);
        glBufferData(GL_ARRAY_BUFFER, b.size() * sizeof(UiVert), b.data(),
                     GL_STREAM_DRAW);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b.size());
    };
    drawBatch(m_uiSolid, m_whiteTex2d);
    drawBatch(m_uiText, m_fontTex);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    m_uiSolid.clear();
    m_uiText.clear();
}

Renderer::Renderer() {
    // Default: the crisp native pack, with retro available to switch to.
    registerShaderPack("native", ShaderPack::standard());
    registerShaderPack("retro", ShaderPack::retro());
    // Compiles "native" now (GL context must be current) — throws on failure.
    if (!useShaderPack("native")) {
        throw std::runtime_error("Renderer: failed to compile native pack");
    }
    buildPipeline();
}

Renderer::Renderer(const ShaderPack& pack) {
    // The built-ins are always available; the passed pack becomes active.
    registerShaderPack("native", ShaderPack::standard());
    registerShaderPack("retro", ShaderPack::retro());
    // Register the explicit pack under its label (or a reserved fallback) and
    // make it active — compiles now, propagating any failure.
    const std::string name =
        pack.label.empty() ? std::string("__explicit__") : pack.label;
    registerShaderPack(name, pack);
    if (!useShaderPack(name)) {
        throw std::runtime_error(
            "Renderer: failed to compile shader pack: " + name);
    }
    buildPipeline();
}

void Renderer::registerShaderPack(const std::string& name, ShaderPack pack) {
    const bool wasActive = (name == m_activeName) && m_active;

    if (wasActive) {
        // Active pack changed under us (hot reload): recompile now so the edit
        // takes effect this frame. Compile the NEW source into a local FIRST so
        // a compile/link throw leaves the previously-active program (m_active,
        // m_compiled, m_packSrc) fully intact — callers that catch the throw
        // can keep rendering the old pack. Only commit on success.
        auto compiled = compilePack(pack); // throws here mutate nothing below
        m_packSrc[name] = std::move(pack);
        auto it = m_compiled.insert_or_assign(name, std::move(compiled)).first;
        m_active = it->second.get();
        return;
    }

    // Not the active pack: store the source and drop any stale compiled program
    // so it lazily recompiles on next use. m_active points at a different pack's
    // node, so erasing this name's entry never dangles it.
    m_packSrc[name] = std::move(pack);
    m_compiled.erase(name);
}

bool Renderer::useShaderPack(const std::string& name) {
    if (name == m_activeName && m_active) return true; // already active

    auto src = m_packSrc.find(name);
    if (src == m_packSrc.end()) return false; // unknown — keep current

    auto comp = m_compiled.find(name);
    if (comp == m_compiled.end()) {
        try {
            comp = m_compiled.emplace(name, compilePack(src->second)).first;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Renderer: shader pack '%s' failed: %s\n",
                         name.c_str(), e.what());
            return false; // keep current active
        }
    }
    m_active = comp->second.get();
    m_activeName = name;
    return true;
}

std::vector<std::string> Renderer::availableShaderPacks() const {
    std::vector<std::string> names;
    names.reserve(m_packSrc.size());
    for (const auto& kv : m_packSrc) names.push_back(kv.first);
    return names;
}

void Renderer::setShaderPack(const ShaderPack& pack) {
    // Back-compat: route through the named registry under a reserved name.
    // Throws on compile failure to preserve the old contract.
    registerShaderPack("__explicit__", pack);
    if (!useShaderPack("__explicit__")) {
        throw std::runtime_error(
            "setShaderPack: failed to compile shader pack: " + pack.label);
    }
}

Renderer::~Renderer() {
    destroyTargets();
    if (m_fsVao) glDeleteVertexArrays(1, &m_fsVao);
    if (m_fsVbo) glDeleteBuffers(1, &m_fsVbo);
    if (m_uiVao) glDeleteVertexArrays(1, &m_uiVao);
    if (m_uiVbo) glDeleteBuffers(1, &m_uiVbo);
    if (m_fontTex) glDeleteTextures(1, &m_fontTex);
    if (m_whiteTex2d) glDeleteTextures(1, &m_whiteTex2d);
    // m_uiShader's dtor frees the GL program.
}

void Renderer::createTargets() {
    // The debug overlay can change the virtual resolution live, so this gets
    // called again mid-run; tear down whatever exists first.
    destroyTargets();

    // Color target is a texture (we need to sample it in the blit pass).
    // GL_NEAREST on BOTH filters is load-bearing: the upscale in endFrame
    // happens via texture sampling, and any linear filtering would smear the
    // chunky pixels into mush and kill the look.
    glGenTextures(1, &m_colorTex);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_renderW,
                 m_renderH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Depth is a renderbuffer, not a texture — nothing ever samples it, and
    // renderbuffers let the driver pick its preferred internal layout.
    // 24-bit depth is plenty for a 0.1..220 frustum.
    glGenRenderbuffers(1, &m_depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                          m_renderW, m_renderH);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_colorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, m_depthRbo);

    // Completeness can fail for format/attachment mismatches that compile
    // fine — always check, but don't abort: a broken FBO just means a black
    // dream, and the message tells us why.
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr,
                     "Renderer: framebuffer incomplete (status 0x%04x) at %dx%d\n",
                     status, m_renderW, m_renderH);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_allocW = m_renderW;
    m_allocH = m_renderH;
}

void Renderer::destroyTargets() {
    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_colorTex) {
        glDeleteTextures(1, &m_colorTex);
        m_colorTex = 0;
    }
    if (m_depthRbo) {
        glDeleteRenderbuffers(1, &m_depthRbo);
        m_depthRbo = 0;
    }
    m_allocW = 0;
    m_allocH = 0;
}

glm::mat4 Renderer::projection() const {
    // Aspect comes from the resolved RENDER resolution — the FBO the scene
    // pass draws into. For retro that is the virtual size (letterboxed blit);
    // for native it is the window size. Fall back to virtual before the first
    // beginFrame has resolved a size. Far plane at 220: the fog has long since
    // eaten everything by then, so anything farther is wasted depth precision.
    const int rw = m_renderW > 0 ? m_renderW : settings.virtualW;
    const int rh = m_renderH > 0 ? m_renderH : settings.virtualH;
    return glm::perspective(glm::radians(settings.fovDegrees),
                            rw / (float)rh, 0.1f, 220.0f);
}

void Renderer::beginFrame(const glm::mat4& view, int windowFbWidth,
                          int windowFbHeight) {
    // Resolve the render size from the active pack: native draws at the window
    // resolution (crisp), retro at the virtual resolution (chunky). Guard
    // against a zero window size (e.g. minimized) by falling back to virtual.
    if (m_active && m_active->res == ShaderResolution::Native &&
        windowFbWidth > 0 && windowFbHeight > 0) {
        m_renderW = windowFbWidth;
        m_renderH = windowFbHeight;
    } else {
        m_renderW = settings.virtualW;
        m_renderH = settings.virtualH;
    }

    // Lazily reallocate the FBO when the resolved size changes — covers both a
    // window resize (native) and the overlay tweaking settings.virtualW/H.
    if (m_allocW != m_renderW || m_allocH != m_renderH) {
        createTargets();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_renderW, m_renderH);

    glEnable(GL_DEPTH_TEST);
    // No face culling, on purpose. The PS1 didn't cull either, and with all
    // geometry procedurally generated (and occasionally wrong, by design)
    // this saves an entire class of winding-order bugs.
    glDisable(GL_CULL_FACE);

    glClearColor(settings.skyColor.r, settings.skyColor.g,
                 settings.skyColor.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Shader& scene = m_active->scene;
    scene.use();
    scene.set("uView", view);
    scene.set("uProj", projection());
    scene.set("uFogColor", settings.fogColor);
    scene.set("uFogDensity", settings.fogDensity);
    scene.set("uLightDir", settings.lightDir);
    scene.set("uShadeDir", settings.shadeDir);
    scene.set("uDecayProgress", settings.decayProgress);
    // Snap grid defaults to one cell per virtual pixel; snapScale < 1 makes
    // the jitter coarser than the framebuffer for an extra-broken feel. The
    // native pack has no uSnapRes uniform; Shader::set no-ops a missing one.
    scene.set("uSnapRes",
              glm::vec2((float)settings.virtualW,
                        (float)settings.virtualH) * settings.snapScale);

    // Lua-set custom uniforms last so they can override anything above. Unknown
    // names are silently ignored by Shader::set (no such active uniform).
    for (const auto& [name, value] : m_customUniforms) {
        std::visit([&](const auto& v) { scene.set(name.c_str(), v); }, value);
    }
}

void Renderer::draw(const DrawItem& item) {
    if (!item.mesh) return;

    Shader& scene = m_active->scene;
    scene.set("uModel", item.model);
    // Normal matrix: inverse-transpose of the model's upper 3x3. Plain
    // mat3(model) would skew normals under the non-uniform scales the spec
    // generator loves to emit, and skewed normals make the (already wrong)
    // lighting wrong in the *un*intended way.
    scene.set("uNormalMat",
              glm::transpose(glm::inverse(glm::mat3(item.model))));
    scene.set("uColor", item.color);
    scene.set("uColor2", item.color2);
    // Gradient runs over the mesh's local height. A flat mesh (height ~0) gets
    // uGradInv = 0, so vGradT pins to 0 and the object is uniformly uColor.
    const float gradH = item.mesh->localMax.y - item.mesh->localMin.y;
    scene.set("uGradBase", item.mesh->localMin.y);
    scene.set("uGradInv", gradH > 1e-4f ? 1.0f / gradH : 0.0f);

    if (item.texture) {
        scene.set("uTex", 0);
        item.texture->bind(0);
    }

    // The one "still" object renders clean: no vertex snap, perspective-
    // correct textures. It holds perfectly steady while the whole world
    // jitters and swims around it — that contrast is the point; the eye
    // finds it without being told to. (Native ignores these missing uniforms.)
    const float snap =
        (settings.vertexSnap && !item.still) ? 1.0f : 0.0f;
    const float affine =
        (settings.affineTextures && !item.still) ? 1.0f : 0.0f;
    scene.set("uSnapEnabled", snap);
    scene.set("uAffine", affine);

    // Set every draw so the cutout flag never leaks between items: an opaque
    // draw following a decal must read 0 even though the shader program holds
    // whatever the last draw left. Default-false on DrawItem means existing
    // call sites push 0.0f here and stay byte-identical to today.
    scene.set("uAlphaTest", item.alphaTest ? 1.0f : 0.0f);
    scene.set("uFogScale", item.fogScale);

    item.mesh->draw();
}

void Renderer::readPixels(std::vector<unsigned char>& rgba, int& w, int& h) const {
    if (m_fbo == 0 || m_allocW == 0 || m_allocH == 0) {
        w = 0;
        h = 0;
        rgba.clear();
        return;
    }
    w = m_allocW;
    h = m_allocH;
    rgba.resize(static_cast<std::size_t>(w) * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::endFrame(int windowFbWidth, int windowFbHeight) {
    // Draw queued 2D UI into the scene FBO while it is STILL bound (before the
    // bind-0 below), so it shows in the blit, the editor FBO image, and
    // screenshots. flushUi clears the batches.
    flushUi();

    // Back to the default framebuffer (the window). From here on we are
    // drawing a single textured triangle; depth testing would only let
    // stale window-depth garbage reject our pixels.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowFbWidth, windowFbHeight);
    glDisable(GL_DEPTH_TEST);

    // Clear the whole window to black first so the letterbox bars are bars
    // and not last frame's leftovers.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Letterbox against the resolved render size: a uniform scale that fits
    // the rendered image inside the window, centered. For native (renderW ==
    // windowFb) scale is 1 and there are no bars; for retro it fits the
    // virtual image inside the window. Float math truncated to ints is fine —
    // being a pixel off-center is invisible, the image stays unstretched.
    const float vw = (float)m_renderW;
    const float vh = (float)m_renderH;
    const float scale =
        std::min(windowFbWidth / vw, windowFbHeight / vh);
    const int rw = (int)(vw * scale);
    const int rh = (int)(vh * scale);
    const int rx = (int)((windowFbWidth - rw) * 0.5f);
    const int ry = (int)((windowFbHeight - rh) * 0.5f);
    glViewport(rx, ry, rw, rh);

    m_active->blit.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    m_active->blit.set("uTex", 0);

    glBindVertexArray(m_fsVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

std::vector<std::string>& shaderCatalog() {
    // Process-global storage the editor/player fill in; Renderer never reads it.
    static std::vector<std::string> c;
    return c;
}

} // namespace liminal
