#pragma once
// Forward renderer chasing the LSD: Dream Emulator look:
//   1. Render the 3D scene into a low-res offscreen framebuffer (~320x240).
//   2. Vertex-snap positions to a coarse grid in the vertex shader (PS1 had
//      no subpixel rasterization — polygons jitter as things move).
//   3. Optional affine (non-perspective-correct) texture mapping.
//   4. Heavy near fog in a tunable color — fog color IS a mood parameter.
//   5. Blit the low-res image to the window with nearest-neighbor upscale.
//
// The renderer knows nothing about the app. The app layer hands it DrawItems
// and mood-flavored settings; that's the whole contract.

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <liminal/render/mesh.hpp>
#include <liminal/render/shader.hpp>
#include <liminal/render/texture.hpp>
#include <liminal/render/types.hpp>

namespace liminal {

// The four GLSL sources the renderer needs: the scene ("dream") pass and the
// low-res -> window blit pass. Default-constructed Renderer uses the embedded
// retro pack; apps override via the explicit ctor or setShaderPack().
struct ShaderPack {
    std::string sceneVert;
    std::string sceneFrag;
    std::string blitVert;
    std::string blitFrag;
    // Names the pack in shader compile/link error messages.
    std::string label = "shader pack";

    // The built-in PS1-style pack, embedded in the library at build time.
    // No filesystem access.
    static ShaderPack retro();
    // Reads the four files; throws std::runtime_error if any cannot be read.
    static ShaderPack fromFiles(const std::string& sceneVertPath,
                                const std::string& sceneFragPath,
                                const std::string& blitVertPath,
                                const std::string& blitFragPath);
};

class Renderer {
public:
    Renderer();                            // embedded retro pack, builds the FBO
    explicit Renderer(const ShaderPack& pack);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Recompiles both programs from the pack (GL context must be current).
    // Throws std::runtime_error on compile/link failure, leaving the
    // previously active programs untouched.
    void setShaderPack(const ShaderPack& pack);

    RenderSettings settings; // live-tweaked by the app between frames

    glm::mat4 projection() const; // perspective from settings + virtual aspect

    // Binds the low-res FBO, clears to sky color, sets shared uniforms.
    void beginFrame(const glm::mat4& view);
    void draw(const DrawItem& item);
    // Resolves to the default framebuffer with nearest-neighbor upscale,
    // preserving the virtual aspect ratio (letterboxed if needed).
    void endFrame(int windowFbWidth, int windowFbHeight);

    // Editor/tooling access: the GL texture name of the low-res color
    // attachment the scene pass renders into. Valid after construction;
    // contents are the last frame drawn between beginFrame()/endFrame().
    // Reallocated (same purpose, possibly new name) when virtualW/H change.
    unsigned int colorTexture() const { return m_colorTex; }

    // Reads the current low-res color FBO into rgba (size = w*h*4, RGBA8,
    // bottom-up). If there is no allocated FBO, sets w=h=0 and clears rgba.
    void readPixels(std::vector<unsigned char>& rgba, int& w, int& h) const;

private:
    void createTargets();  // (re)allocate FBO at settings.virtualW/H
    void destroyTargets();

    Shader m_dreamShader;
    Shader m_blitShader;

    unsigned int m_fbo = 0;
    unsigned int m_colorTex = 0;
    unsigned int m_depthRbo = 0;
    int m_allocW = 0, m_allocH = 0;

    unsigned int m_fsVao = 0; // fullscreen triangle
    unsigned int m_fsVbo = 0;

    glm::mat4 m_view{1.0f};
};

} // namespace liminal
