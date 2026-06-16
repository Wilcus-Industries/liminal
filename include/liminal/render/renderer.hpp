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

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

#include <liminal/render/mesh.hpp>
#include <liminal/render/shader.hpp>
#include <liminal/render/texture.hpp>
#include <liminal/render/types.hpp>

namespace liminal {

// Whether a pack renders the scene at the full window resolution (crisp) or
// into the low-res virtual FBO (chunky PS1 look). Drives the FBO size in
// beginFrame and the letterbox math in endFrame.
enum class ShaderResolution { Native, LowRes };

// The four GLSL sources the renderer needs: the scene ("dream") pass and the
// low-res -> window blit pass. Default-constructed Renderer uses the embedded
// native pack; apps override via the explicit ctor or setShaderPack().
struct ShaderPack {
    std::string sceneVert;
    std::string sceneFrag;
    std::string blitVert;
    std::string blitFrag;
    // Names the pack in shader compile/link error messages.
    std::string label = "shader pack";
    // Native = render at window resolution; LowRes = the virtual FBO.
    ShaderResolution res = ShaderResolution::Native;

    // The built-in PS1-style low-res pack, embedded at build time.
    // No filesystem access.
    static ShaderPack retro();
    // The built-in crisp full-resolution pack, embedded at build time.
    static ShaderPack standard();
    // Reads the four files; throws std::runtime_error if any cannot be read.
    static ShaderPack fromFiles(const std::string& sceneVertPath,
                                const std::string& sceneFragPath,
                                const std::string& blitVertPath,
                                const std::string& blitFragPath);

    // Build a Native pack from in-memory custom scene sources, reusing the
    // shared (native) blit stage. `label`/name handling is the caller's job.
    // These back both the editor's on-disk shader discovery and the player's
    // pak-based discovery so the frag-only wrap contract lives in one place.

    // Full custom pack: caller-provided scene vertex + fragment sources.
    static ShaderPack makeFullPack(const std::string& sceneVert,
                                   const std::string& sceneFrag);
    // Frag-only pack: `fragBody` is a fragment BODY ONLY (no #version, no
    // in/out/uniform decls — just the documented ins/uniforms + `void main`).
    // The engine prepends the native frag header and supplies the native
    // vertex stage so the wrapped fragment links against the same varyings.
    static ShaderPack makeFragOnlyPack(const std::string& fragBody);
};

class Renderer {
public:
    Renderer();                            // embedded native pack, builds the FBO
    explicit Renderer(const ShaderPack& pack);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Recompiles both programs from the pack (GL context must be current).
    // Throws std::runtime_error on compile/link failure, leaving the
    // previously active programs untouched. Kept for existing callers; backed
    // by the named registry below under a reserved "__explicit__" name.
    void setShaderPack(const ShaderPack& pack);

    // Named shader registry. Packs register their sources; compilation is
    // lazy (deferred to the first useShaderPack switch). The built-ins
    // "native" and "retro" are always registered by the ctor.

    // Stores the pack under name (overwrite allowed). Drops any previously
    // compiled program for that name so it recompiles on next use; if it is
    // the currently active pack, recompiles immediately (hot reload).
    void registerShaderPack(const std::string& name, ShaderPack pack);
    // Switches the active pack by name. No-op + true if already active.
    // Compiles lazily on first switch; on compile failure prints to stderr
    // and returns false, keeping the current pack. Returns false (no change)
    // if the name is not registered.
    bool useShaderPack(const std::string& name);
    // Registered pack names (compiled or not).
    std::vector<std::string> availableShaderPacks() const;
    const std::string& activeShaderPack() const { return m_activeName; }

    RenderSettings settings; // live-tweaked by the app between frames

    // Lua-settable render knobs (G3). The first five write into `settings` so
    // they feed the same per-frame uniform sets beginFrame already does; they
    // exist so scripts (lm.render) can drive fog/decay/lights without a custom
    // uniform name. setUniform stores an arbitrary named uniform applied to the
    // active scene shader each beginFrame (after the fixed sets) — Shader::set
    // no-ops names the active program doesn't declare.
    void setFogColor(const glm::vec3& c) { settings.fogColor = c; }
    void setFogDensity(float d) { settings.fogDensity = d; }
    void setDecay(float p) { settings.decayProgress = p; }
    void setLightDir(const glm::vec3& d) { settings.lightDir = d; }
    void setShadeDir(const glm::vec3& d) { settings.shadeDir = d; }
    void setUniform(const std::string& name, float v) { m_customUniforms[name] = v; }
    void setUniform(const std::string& name, const glm::vec3& v) { m_customUniforms[name] = v; }

    glm::mat4 projection() const; // perspective from settings + render aspect

    // Binds the render FBO (window-sized for native, virtual for retro),
    // clears to sky color, sets shared uniforms. windowFb* sizes the FBO for
    // native packs; retro ignores them and uses settings.virtualW/H.
    void beginFrame(const glm::mat4& view, int windowFbWidth,
                    int windowFbHeight);
    void draw(const DrawItem& item);
    // Resolves to the default framebuffer with nearest-neighbor upscale,
    // preserving the virtual aspect ratio (letterboxed if needed).
    void endFrame(int windowFbWidth, int windowFbHeight);

    // Editor/tooling access: the GL texture name of the color attachment the
    // scene pass renders into (window resolution for native packs, the virtual
    // FBO for retro). Valid after construction; contents are the last frame
    // drawn between beginFrame()/endFrame(). Reallocated (same purpose,
    // possibly new name) when the resolved render size changes.
    unsigned int colorTexture() const { return m_colorTex; }

    // Reads the current color FBO into rgba (size = w*h*4, RGBA8, bottom-up;
    // window resolution for native packs, the virtual FBO for retro). If there
    // is no allocated FBO, sets w=h=0 and clears rgba.
    void readPixels(std::vector<unsigned char>& rgba, int& w, int& h) const;

    // Immediate-mode screen-space 2D (origin top-left, render-target pixels).
    // Queued each frame and flushed into the render FBO at the top of endFrame
    // (so it shows in the player blit, the editor FBO image, and screenshots).
    // Backs lm.ui. color is RGBA 0..1.
    void uiText(float x, float y, const std::string& text,
                const glm::vec4& color, float scale = 1.0f);
    void uiRect(float x, float y, float w, float h, const glm::vec4& color);
    void uiLine(float x0, float y0, float x1, float y1,
                const glm::vec4& color, float thickness = 1.0f);
    void uiSize(int& w, int& h) const { w = m_renderW; h = m_renderH; }

    // A registered pack's compiled programs plus the resolution it renders at.
    // Aggregate: Shader is move-only, so brace-init with moved Shaders works.
    // Public only so the .cpp's compile helper can name it; not part of the
    // intended API surface.
    struct CompiledShader {
        Shader scene;
        Shader blit;
        ShaderResolution res;
    };

private:
    void createTargets();   // (re)allocate FBO at m_renderW/m_renderH
    void destroyTargets();
    void buildPipeline();   // shared ctor GL setup (FBO + blit triangle)

    std::unordered_map<std::string, ShaderPack> m_packSrc;       // by name
    std::unordered_map<std::string, std::unique_ptr<CompiledShader>>
        m_compiled;                                              // lazy
    CompiledShader* m_active = nullptr;
    std::string m_activeName;

    std::unordered_map<std::string, std::variant<float, glm::vec3>>
        m_customUniforms; // applied to the active scene shader each beginFrame

    unsigned int m_fbo = 0;
    unsigned int m_colorTex = 0;
    unsigned int m_depthRbo = 0;
    int m_allocW = 0, m_allocH = 0;
    int m_renderW = 0, m_renderH = 0; // resolved render size for the active pack

    unsigned int m_fsVao = 0; // fullscreen triangle
    unsigned int m_fsVbo = 0;

    // Immediate-mode 2D UI. Two batches (solid white-texel vs glyph atlas)
    // queued by uiRect/uiLine/uiText and flushed into the FBO by flushUi at the
    // top of endFrame, while the scene FBO is still bound.
    struct UiVert { float x, y, u, v, r, g, b, a; };
    static void pushQuad(std::vector<UiVert>& batch, float x0, float y0,
                         float x1, float y1, float u0, float v0, float u1,
                         float v1, const glm::vec4& c);
    void flushUi();          // draws m_uiSolid then m_uiText into the bound FBO
    void buildUiResources(); // font atlas + 1x1 white tex + 2D program + VAO/VBO
    std::vector<UiVert> m_uiSolid; // rects/lines (white texel)
    std::vector<UiVert> m_uiText;  // glyph quads (font atlas)
    std::unique_ptr<Shader> m_uiShader;
    unsigned int m_fontTex = 0;
    unsigned int m_whiteTex2d = 0;
    unsigned int m_uiVao = 0, m_uiVbo = 0;
    int m_fontAtlasW = 0, m_fontAtlasH = 0; // px
};

// Process-global list of selectable shader pack names. The renderer does not
// read this; the editor/player populate it so UI (the Camera inspector
// dropdown) can offer the same set the renderer was registered with.
std::vector<std::string>& shaderCatalog();

} // namespace liminal
