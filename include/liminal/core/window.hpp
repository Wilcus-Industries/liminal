#pragma once
// Thin RAII wrapper around GLFW: one window, one GL context, input state.
// The engine layer knows nothing about the game; it only reports raw input.

#include <functional>
#include <string>

struct GLFWwindow;

namespace liminal {

class Window {
public:
    // Creates the window and a core-profile GL context (version injected by
    // CMake: 4.1 on macOS, 4.6 elsewhere), makes it current, loads GL via glad.
    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void requestClose();
    void pollEvents();
    void swapBuffers();
    // Vsync on (1) by default — the dream does not tear. Off (0) lets the loop
    // spin free of the compositor, which matters for headless autopilot: an
    // occluded vsync'd window throttles swapBuffers to a near-halt.
    void setVsync(bool on);

    // Window size in screen coordinates; framebuffer size in pixels.
    // On retina displays these differ — always use framebuffer size for GL.
    void framebufferSize(int& w, int& h) const;

    bool keyDown(int glfwKey) const;
    // True only on the frame the key went down (edge detection).
    bool keyPressed(int glfwKey);

    bool mouseDown(int glfwButton) const;
    // True only on the frame the button went down (edge detection).
    bool mousePressed(int glfwButton);
    // Accumulated vertical wheel motion since the last call, then zeroed.
    float scrollDelta();

    // Mouse look: when captured, the cursor is hidden/locked and we
    // accumulate deltas. When not captured, deltas are zero.
    void setCursorCaptured(bool captured);
    bool cursorCaptured() const { return m_captured; }
    void mouseDelta(float& dx, float& dy);

    double time() const;
    GLFWwindow* handle() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
    bool m_captured = false;
    bool m_firstMouse = true;
    double m_lastX = 0.0, m_lastY = 0.0;
    float m_accumDX = 0.0f, m_accumDY = 0.0f;
    float m_accumScroll = 0.0f;
    bool m_prevKey[512] = {};
    bool m_prevMouse[8] = {};
};

} // namespace liminal
