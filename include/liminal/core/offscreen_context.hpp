#pragma once
// Display-less GL context creation for headless mode: a real OpenGL context with
// NO window, NO surface, and NO display server (X11/Wayland/WindowServer).
//
// Two backends, chosen at configure time; the whole class is absent from a
// default build (the guard below collapses to nothing unless one is enabled):
//   LIMINAL_HEADLESS_EGL    — EGL surfaceless / device platform. Portable: a real
//                             GPU when present (NVIDIA / Mesa HW), llvmpipe
//                             software otherwise. Works on Linux AND on macOS via
//                             Homebrew Mesa's libEGL. The primary backend.
//   LIMINAL_HEADLESS_OSMESA — OSMesa pure-CPU rasteriser. For boxes that ship
//                             libOSMesa but no usable EGL.
//
// The engine renders into its own FBO and reads it back with glReadPixels, so no
// presentable surface is ever needed — this only swaps how the context is born.

#if defined(LIMINAL_HEADLESS_EGL) || defined(LIMINAL_HEADLESS_OSMESA)
#define LIMINAL_HAS_OFFSCREEN 1

#include <cstdint>
#include <vector>

namespace liminal {

// RAII offscreen GL context. The ctor creates the context and makes it current
// on the constructing thread (throws std::runtime_error on failure); the dtor
// releases it. One per process — headless creates exactly one.
class OffscreenContext {
public:
    OffscreenContext(int width, int height);
    ~OffscreenContext();

    OffscreenContext(const OffscreenContext&) = delete;
    OffscreenContext& operator=(const OffscreenContext&) = delete;

    // GL entry-point loader for gladLoadGL(...). The signature matches
    // GLADloadfunc (void* (*)(const char*)), so the existing desktop-GL glad
    // loader is reused verbatim — no glad regeneration for EGL/OSMesa.
    using LoadFunc = void* (*)(const char*);
    static LoadFunc loader();

    // "EGL" / "OSMesa" — for the startup log line.
    static const char* backendName();

private:
#if defined(LIMINAL_HEADLESS_OSMESA)
    std::vector<std::uint8_t> m_buffer; // OSMesa's "default framebuffer" (unused)
    void* m_ctx = nullptr;              // OSMesaContext
#elif defined(LIMINAL_HEADLESS_EGL)
    void* m_display = nullptr; // EGLDisplay
    void* m_context = nullptr; // EGLContext
#endif
};

} // namespace liminal

#endif // offscreen backend enabled
