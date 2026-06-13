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
#include <sstream>
#include <stdexcept>
#include <utility>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

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

Renderer::Renderer() : Renderer(ShaderPack::retro()) {}

Renderer::Renderer(const ShaderPack& pack)
    // Shader compiles throw on failure — acceptable here, construction-only.
    : m_dreamShader{Shader::fromSource(pack.sceneVert, pack.sceneFrag,
                                       pack.label + " [scene]")},
      m_blitShader{Shader::fromSource(pack.blitVert, pack.blitFrag,
                                      pack.label + " [blit]")} {
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
        destroyTargets();
        throw;
    }
}

void Renderer::setShaderPack(const ShaderPack& pack) {
    // Compile both before assigning either, so a failed pack leaves the
    // renderer on its previous (working) programs.
    Shader dream = Shader::fromSource(pack.sceneVert, pack.sceneFrag,
                                      pack.label + " [scene]");
    Shader blit = Shader::fromSource(pack.blitVert, pack.blitFrag,
                                     pack.label + " [blit]");
    m_dreamShader = std::move(dream);
    m_blitShader = std::move(blit);
}

Renderer::~Renderer() {
    destroyTargets();
    if (m_fsVao) glDeleteVertexArrays(1, &m_fsVao);
    if (m_fsVbo) glDeleteBuffers(1, &m_fsVbo);
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, settings.virtualW,
                 settings.virtualH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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
                          settings.virtualW, settings.virtualH);
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
                     status, settings.virtualW, settings.virtualH);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_allocW = settings.virtualW;
    m_allocH = settings.virtualH;
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
    // Aspect comes from the VIRTUAL resolution — the window only ever sees
    // the letterboxed blit, so the projection must match the FBO, not the
    // window. Far plane at 220: the fog has long since eaten everything by
    // then, so anything farther is wasted depth precision.
    return glm::perspective(glm::radians(settings.fovDegrees),
                            settings.virtualW / (float)settings.virtualH,
                            0.1f, 220.0f);
}

void Renderer::beginFrame(const glm::mat4& view) {
    // The overlay tweaks settings.virtualW/H directly; lazily reallocate the
    // FBO here rather than making the overlay know about GL objects.
    if (m_allocW != settings.virtualW || m_allocH != settings.virtualH) {
        createTargets();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, settings.virtualW, settings.virtualH);

    glEnable(GL_DEPTH_TEST);
    // No face culling, on purpose. The PS1 didn't cull either, and with all
    // geometry procedurally generated (and occasionally wrong, by design)
    // this saves an entire class of winding-order bugs.
    glDisable(GL_CULL_FACE);

    glClearColor(settings.skyColor.r, settings.skyColor.g,
                 settings.skyColor.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_dreamShader.use();
    m_dreamShader.set("uView", view);
    m_dreamShader.set("uProj", projection());
    m_dreamShader.set("uFogColor", settings.fogColor);
    m_dreamShader.set("uFogDensity", settings.fogDensity);
    m_dreamShader.set("uLightDir", settings.lightDir);
    m_dreamShader.set("uShadeDir", settings.shadeDir);
    m_dreamShader.set("uDecayProgress", settings.decayProgress);
    // Snap grid defaults to one cell per virtual pixel; snapScale < 1 makes
    // the jitter coarser than the framebuffer for an extra-broken feel.
    m_dreamShader.set("uSnapRes",
                      glm::vec2((float)settings.virtualW,
                                (float)settings.virtualH) * settings.snapScale);

    m_view = view;
}

void Renderer::draw(const DrawItem& item) {
    if (!item.mesh) return;

    m_dreamShader.set("uModel", item.model);
    // Normal matrix: inverse-transpose of the model's upper 3x3. Plain
    // mat3(model) would skew normals under the non-uniform scales the spec
    // generator loves to emit, and skewed normals make the (already wrong)
    // lighting wrong in the *un*intended way.
    m_dreamShader.set("uNormalMat",
                      glm::transpose(glm::inverse(glm::mat3(item.model))));
    m_dreamShader.set("uColor", item.color);
    m_dreamShader.set("uColor2", item.color2);
    // Gradient runs over the mesh's local height. A flat mesh (height ~0) gets
    // uGradInv = 0, so vGradT pins to 0 and the object is uniformly uColor.
    const float gradH = item.mesh->localMax.y - item.mesh->localMin.y;
    m_dreamShader.set("uGradBase", item.mesh->localMin.y);
    m_dreamShader.set("uGradInv", gradH > 1e-4f ? 1.0f / gradH : 0.0f);

    if (item.texture) {
        m_dreamShader.set("uTex", 0);
        item.texture->bind(0);
    }

    // The one "still" object renders clean: no vertex snap, perspective-
    // correct textures. It holds perfectly steady while the whole world
    // jitters and swims around it — that contrast is the point; the eye
    // finds it without being told to.
    const float snap =
        (settings.vertexSnap && !item.still) ? 1.0f : 0.0f;
    const float affine =
        (settings.affineTextures && !item.still) ? 1.0f : 0.0f;
    m_dreamShader.set("uSnapEnabled", snap);
    m_dreamShader.set("uAffine", affine);

    // Set every draw so the cutout flag never leaks between items: an opaque
    // draw following a decal must read 0 even though the shader program holds
    // whatever the last draw left. Default-false on DrawItem means existing
    // call sites push 0.0f here and stay byte-identical to today.
    m_dreamShader.set("uAlphaTest", item.alphaTest ? 1.0f : 0.0f);
    m_dreamShader.set("uFogScale", item.fogScale);

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

    // Letterbox: uniform scale that fits the virtual image inside the
    // window, centered. Float math truncated to ints is fine — being a
    // pixel off-center is invisible, and the image itself stays unstretched.
    const float vw = (float)settings.virtualW;
    const float vh = (float)settings.virtualH;
    const float scale =
        std::min(windowFbWidth / vw, windowFbHeight / vh);
    const int rw = (int)(vw * scale);
    const int rh = (int)(vh * scale);
    const int rx = (int)((windowFbWidth - rw) * 0.5f);
    const int ry = (int)((windowFbHeight - rh) * 0.5f);
    glViewport(rx, ry, rw, rh);

    m_blitShader.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    m_blitShader.set("uTex", 0);

    glBindVertexArray(m_fsVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

} // namespace liminal
