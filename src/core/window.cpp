#include <liminal/core/window.hpp>

#include <chrono>
#include <cstdio>
#include <stdexcept>

// glad must come before GLFW so GLFW doesn't pull in system GL headers.
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <liminal/core/offscreen_context.hpp> // empty unless a backend is enabled

namespace liminal {

namespace {
// Monotonic clock for the offscreen time() base (GLFW's clock is unavailable
// when we never call glfwInit).
long long nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

Window::Window(int width, int height, const std::string& title, bool visible,
               bool offscreen) {
#ifdef LIMINAL_HAS_OFFSCREEN
    if (offscreen) {
        // Display-less path: create a surfaceless/CPU GL context, no GLFW, no
        // display server. On failure, fall through to the hidden-window path.
        try {
            m_offCtx = new OffscreenContext(width, height);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "[gl] offscreen context unavailable (%s); "
                         "falling back to a hidden GLFW window\n",
                         e.what());
            m_offCtx = nullptr;
        }
        if (m_offCtx) {
            if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(
                    OffscreenContext::loader()))) {
                delete m_offCtx;
                m_offCtx = nullptr;
                throw std::runtime_error("gladLoadGL failed (offscreen context)");
            }
            m_offscreen = true;
            m_offW = width;
            m_offH = height;
            m_offStartNs = nowNs();
            std::printf("[gl] %s | %s (offscreen: %s)\n",
                        glGetString(GL_VERSION), glGetString(GL_RENDERER),
                        OffscreenContext::backendName());
            return;
        }
    }
#else
    (void)offscreen;
#endif

    if (!glfwInit()) {
        throw std::runtime_error("glfwInit failed");
    }

    // Request exactly the core profile we generated the glad loader for.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, LIMINAL_GL_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, LIMINAL_GL_MINOR);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // macOS refuses to create a >2.1 context without forward compatibility.
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    // Headless: create the window hidden. The context + offscreen FBO still work
    // for rendering and screenshots; nothing is painted to a visible surface.
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed (GL version unsupported?)");
    }
    glfwMakeContextCurrent(m_window);
    // Vsync on only when visible — a hidden/occluded vsync'd window throttles to
    // a near-halt (see Window::setVsync), which would stall the headless loop.
    glfwSwapInterval(visible ? 1 : 0);

    // glad2 resolves every GL entry point through GLFW's platform loader.
    if (!gladLoadGL(glfwGetProcAddress)) {
        glfwDestroyWindow(m_window);
        glfwTerminate();
        throw std::runtime_error("gladLoadGL failed");
    }
    std::printf("[gl] %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    glfwSetWindowUserPointer(m_window, this);
    // Scroll arrives via callback only. ImGui's GLFW backend installs its own
    // callback later (DebugOverlay is constructed after Window) and chains to
    // this one, so both see the wheel.
    glfwSetScrollCallback(m_window, [](GLFWwindow* w, double, double yoff) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (self) self->m_accumScroll += float(yoff);
    });
    if (glfwRawMouseMotionSupported()) {
        // Raw motion skips OS pointer acceleration — steadier mouse look.
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
}

Window::~Window() {
#ifdef LIMINAL_HAS_OFFSCREEN
    if (m_offscreen) {
        delete m_offCtx; // releases the GL context; no GLFW was ever started
        return;
    }
#endif
    if (m_window) glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool Window::shouldClose() const {
    if (m_offscreen) return m_closeFlag;
    return glfwWindowShouldClose(m_window);
}
void Window::requestClose() {
    if (m_offscreen) {
        m_closeFlag = true;
        return;
    }
    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

void Window::pollEvents() {
    if (m_offscreen) return; // no event source without a window
    glfwPollEvents();

    // Accumulate mouse deltas while captured; the game consumes them once
    // per simulation tick. Polling (vs cursor callbacks) keeps the engine
    // free of game-facing callback plumbing.
    double x, y;
    glfwGetCursorPos(m_window, &x, &y);
    if (m_captured) {
        if (m_firstMouse) {
            m_lastX = x;
            m_lastY = y;
            m_firstMouse = false;
        }
        m_accumDX += float(x - m_lastX);
        m_accumDY += float(y - m_lastY);
    }
    m_lastX = x;
    m_lastY = y;
}

void Window::swapBuffers() {
    if (m_offscreen) return; // nothing to present
    glfwSwapBuffers(m_window);
}

void Window::setVsync(bool on) {
    if (m_offscreen) return;
    glfwSwapInterval(on ? 1 : 0);
}

void Window::framebufferSize(int& w, int& h) const {
    if (m_offscreen) {
        w = m_offW;
        h = m_offH;
        return;
    }
    glfwGetFramebufferSize(m_window, &w, &h);
}

bool Window::keyDown(int key) const {
    if (key < 0 || key > GLFW_KEY_LAST) return false;
    if (key < 512 && m_synthKey[key]) return true; // agent-held overrides
    if (m_offscreen) return false;
    return glfwGetKey(m_window, key) == GLFW_PRESS;
}

bool Window::keyPressed(int key) {
    if (m_offscreen) return false;
    if (key < 0 || key >= 512) return false;
    bool down = keyDown(key);
    bool pressed = down && !m_prevKey[key];
    m_prevKey[key] = down;
    return pressed;
}

bool Window::mouseDown(int button) const {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST) return false;
    if (button < 8 && m_synthMouse[button]) return true; // agent-held overrides
    if (m_offscreen) return false;
    return glfwGetMouseButton(m_window, button) == GLFW_PRESS;
}

bool Window::mousePressed(int button) {
    if (m_offscreen) return false;
    if (button < 0 || button >= 8) return false;
    bool down = mouseDown(button);
    bool pressed = down && !m_prevMouse[button];
    m_prevMouse[button] = down;
    return pressed;
}

float Window::scrollDelta() {
    if (m_offscreen) return 0.0f;
    float s = m_accumScroll;
    m_accumScroll = 0.0f;
    return s;
}

void Window::setCursorCaptured(bool captured) {
    if (m_offscreen) return;
    if (captured == m_captured) return;
    m_captured = captured;
    m_firstMouse = true; // avoid a huge delta on the first captured frame
    glfwSetInputMode(m_window, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Window::mouseDelta(float& dx, float& dy) {
    // Real accum is zero offscreen; the synthetic look delta is added either way
    // so an agent can drive mouse-look with or without a window.
    dx = m_accumDX + m_synthDX;
    dy = m_accumDY + m_synthDY;
    m_accumDX = 0.0f;
    m_accumDY = 0.0f;
    m_synthDX = 0.0f;
    m_synthDY = 0.0f;
}

void Window::setSyntheticKey(int key, bool down, double holdSeconds) {
    if (key < 0 || key >= 512) return;
    m_synthKey[key] = down;
    m_synthKeyDeadline[key] =
        (down && holdSeconds > 0.0) ? time() + holdSeconds : 0.0;
}

void Window::setSyntheticMouse(int button, bool down, double holdSeconds) {
    if (button < 0 || button >= 8) return;
    m_synthMouse[button] = down;
    m_synthMouseDeadline[button] =
        (down && holdSeconds > 0.0) ? time() + holdSeconds : 0.0;
}

void Window::injectMouseLook(float dx, float dy) {
    m_synthDX += dx;
    m_synthDY += dy;
}

void Window::clearSyntheticInput() {
    for (int i = 0; i < 512; ++i) {
        m_synthKey[i] = false;
        m_synthKeyDeadline[i] = 0.0;
    }
    for (int i = 0; i < 8; ++i) {
        m_synthMouse[i] = false;
        m_synthMouseDeadline[i] = 0.0;
    }
    m_synthDX = m_synthDY = 0.0f;
    m_synthCapture = false;
}

void Window::tickSyntheticInput(double now) {
    for (int i = 0; i < 512; ++i) {
        if (m_synthKey[i] && m_synthKeyDeadline[i] > 0.0 &&
            now >= m_synthKeyDeadline[i]) {
            m_synthKey[i] = false;
            m_synthKeyDeadline[i] = 0.0;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (m_synthMouse[i] && m_synthMouseDeadline[i] > 0.0 &&
            now >= m_synthMouseDeadline[i]) {
            m_synthMouse[i] = false;
            m_synthMouseDeadline[i] = 0.0;
        }
    }
}

double Window::time() const {
    if (m_offscreen) return double(nowNs() - m_offStartNs) / 1e9;
    return glfwGetTime();
}

} // namespace liminal
