#include <liminal/core/window.hpp>

#include <cstdio>
#include <stdexcept>

// glad must come before GLFW so GLFW doesn't pull in system GL headers.
#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace liminal {

Window::Window(int width, int height, const std::string& title) {
    if (!glfwInit()) {
        throw std::runtime_error("glfwInit failed");
    }

    // Request exactly the core profile we generated the glad loader for.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, LIMINAL_GL_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, LIMINAL_GL_MINOR);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // macOS refuses to create a >2.1 context without forward compatibility.
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed (GL version unsupported?)");
    }
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // vsync — the dream does not tear

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
    if (m_window) glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(m_window); }
void Window::requestClose() { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }

void Window::pollEvents() {
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

void Window::swapBuffers() { glfwSwapBuffers(m_window); }

void Window::setVsync(bool on) { glfwSwapInterval(on ? 1 : 0); }

void Window::framebufferSize(int& w, int& h) const {
    glfwGetFramebufferSize(m_window, &w, &h);
}

bool Window::keyDown(int key) const {
    if (key < 0 || key > GLFW_KEY_LAST) return false;
    return glfwGetKey(m_window, key) == GLFW_PRESS;
}

bool Window::keyPressed(int key) {
    if (key < 0 || key >= 512) return false;
    bool down = keyDown(key);
    bool pressed = down && !m_prevKey[key];
    m_prevKey[key] = down;
    return pressed;
}

bool Window::mouseDown(int button) const {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST) return false;
    return glfwGetMouseButton(m_window, button) == GLFW_PRESS;
}

bool Window::mousePressed(int button) {
    if (button < 0 || button >= 8) return false;
    bool down = mouseDown(button);
    bool pressed = down && !m_prevMouse[button];
    m_prevMouse[button] = down;
    return pressed;
}

float Window::scrollDelta() {
    float s = m_accumScroll;
    m_accumScroll = 0.0f;
    return s;
}

void Window::setCursorCaptured(bool captured) {
    if (captured == m_captured) return;
    m_captured = captured;
    m_firstMouse = true; // avoid a huge delta on the first captured frame
    glfwSetInputMode(m_window, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Window::mouseDelta(float& dx, float& dy) {
    dx = m_accumDX;
    dy = m_accumDY;
    m_accumDX = 0.0f;
    m_accumDY = 0.0f;
}

double Window::time() const { return glfwGetTime(); }

} // namespace liminal
