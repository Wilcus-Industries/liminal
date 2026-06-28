#include <liminal/core/offscreen_context.hpp>

#ifdef LIMINAL_HAS_OFFSCREEN

#include <stdexcept>
#include <string>

#if defined(LIMINAL_HEADLESS_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#elif defined(LIMINAL_HEADLESS_OSMESA)
#include <GL/osmesa.h>
#endif

namespace liminal {

#if defined(LIMINAL_HEADLESS_EGL)

namespace {
// Open a display with no window system, preferring real hardware:
//   1) device platform (EGL_EXT_platform_device): bind a GPU directly, no X.
//   2) Mesa surfaceless: llvmpipe software when there's no GPU (also macOS).
//   3) plain default display as a last resort.
EGLDisplay openDisplay() {
    auto queryDevices =
        (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    auto getPlatDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (queryDevices && getPlatDisplayEXT) {
        EGLDeviceEXT devs[16];
        EGLint n = 0;
        if (queryDevices(16, devs, &n) && n > 0) {
            for (EGLint i = 0; i < n; ++i) {
                EGLDisplay d = getPlatDisplayEXT(EGL_PLATFORM_DEVICE_EXT,
                                                 devs[i], nullptr);
                if (d != EGL_NO_DISPLAY) return d;
            }
        }
    }
    auto getPlatDisplay =
        (PFNEGLGETPLATFORMDISPLAYPROC)eglGetProcAddress("eglGetPlatformDisplay");
    if (getPlatDisplay) {
        // native_display is void* here; EGL_DEFAULT_DISPLAY is an integer 0
        // sentinel (surfaceless ignores it anyway).
        EGLDisplay d = getPlatDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                      reinterpret_cast<void*>(EGL_DEFAULT_DISPLAY),
                                      nullptr);
        if (d != EGL_NO_DISPLAY) return d;
    }
    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}
} // namespace

OffscreenContext::OffscreenContext(int, int) {
    EGLDisplay dpy = openDisplay();
    if (dpy == EGL_NO_DISPLAY)
        throw std::runtime_error("offscreen EGL: no display available");

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor))
        throw std::runtime_error("offscreen EGL: eglInitialize failed");

    // Desktop GL (not GLES) — the engine's shaders target core-profile 4.x.
    if (!eglBindAPI(EGL_OPENGL_API))
        throw std::runtime_error("offscreen EGL: eglBindAPI(EGL_OPENGL_API) failed");

    // Try to match a config; if the driver advertises none we fall back to a
    // no-config context (EGL_KHR_no_config_context). Either way the context is
    // surfaceless — we only ever render into our own FBO.
    EGLConfig cfg = EGL_NO_CONFIG_KHR;
    const EGLint cfgAttrs[] = {EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                               EGL_NONE};
    EGLint nc = 0;
    eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &nc);
    if (nc == 0) cfg = EGL_NO_CONFIG_KHR;

    const EGLint ctxAttrs[] = {
        EGL_CONTEXT_MAJOR_VERSION,        LIMINAL_GL_MAJOR,
        EGL_CONTEXT_MINOR_VERSION,        LIMINAL_GL_MINOR,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,  EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT) {
        eglTerminate(dpy);
        throw std::runtime_error("offscreen EGL: eglCreateContext failed");
    }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        throw std::runtime_error("offscreen EGL: eglMakeCurrent failed");
    }

    m_display = dpy;
    m_context = ctx;
}

OffscreenContext::~OffscreenContext() {
    auto dpy = static_cast<EGLDisplay>(m_display);
    if (dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context) eglDestroyContext(dpy, static_cast<EGLContext>(m_context));
        eglTerminate(dpy);
    }
}

OffscreenContext::LoadFunc OffscreenContext::loader() {
    return [](const char* name) -> void* {
        return reinterpret_cast<void*>(eglGetProcAddress(name));
    };
}

const char* OffscreenContext::backendName() { return "EGL"; }

#elif defined(LIMINAL_HEADLESS_OSMESA)

OffscreenContext::OffscreenContext(int width, int height) {
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    const int attribs[] = {OSMESA_FORMAT,                OSMESA_RGBA,
                           OSMESA_DEPTH_BITS,            24,
                           OSMESA_STENCIL_BITS,          8,
                           OSMESA_PROFILE,               OSMESA_CORE_PROFILE,
                           OSMESA_CONTEXT_MAJOR_VERSION, LIMINAL_GL_MAJOR,
                           OSMESA_CONTEXT_MINOR_VERSION, LIMINAL_GL_MINOR,
                           0};
    OSMesaContext ctx = OSMesaCreateContextAttribs(attribs, nullptr);
    if (!ctx)
        throw std::runtime_error("offscreen OSMesa: OSMesaCreateContextAttribs failed");

    // OSMesa needs a client buffer to make current; the engine renders into its
    // own FBO and reads that back, so this buffer is just a parking spot.
    m_buffer.assign(static_cast<std::size_t>(width) * height * 4, 0);
    if (!OSMesaMakeCurrent(ctx, m_buffer.data(), GL_UNSIGNED_BYTE, width, height)) {
        OSMesaDestroyContext(ctx);
        throw std::runtime_error("offscreen OSMesa: OSMesaMakeCurrent failed");
    }
    m_ctx = ctx;
}

OffscreenContext::~OffscreenContext() {
    if (m_ctx) OSMesaDestroyContext(static_cast<OSMesaContext>(m_ctx));
}

OffscreenContext::LoadFunc OffscreenContext::loader() {
    return [](const char* name) -> void* {
        return reinterpret_cast<void*>(OSMesaGetProcAddress(name));
    };
}

const char* OffscreenContext::backendName() { return "OSMesa"; }

#endif

} // namespace liminal

#endif // LIMINAL_HAS_OFFSCREEN
