#pragma once
// Thin RAII wrapper around GLFW: one window, one GL context, input state.
// The engine layer knows nothing about the game; it only reports raw input.

#include <functional>
#include <string>

struct GLFWwindow;

namespace liminal {

class OffscreenContext; // display-less GL context (offscreen_context.hpp)

class Window {
public:
    // Creates the window and a core-profile GL context (version injected by
    // CMake: 4.1 on macOS, 4.6 elsewhere), makes it current, loads GL via glad.
    // visible=false hides the window (GLFW_VISIBLE off) and disables vsync — the
    // headless path still gets a real GL context + offscreen FBO (so the
    // Renderer and readPixels/screenshot work) without painting to screen.
    //
    // offscreen=true creates a DISPLAY-LESS context (EGL surfaceless / OSMesa,
    // see OffscreenContext) — no GLFW window, no display server at all — for
    // truly headless boxes (CI, cloud VMs, bare SSH). The GL context + FBO still
    // work, so screenshots and every MCP tool are unaffected; all windowing/input
    // methods become no-ops (handle() is null, input reads return none). If no
    // offscreen backend was compiled in, the request falls back to a hidden GLFW
    // window so --headless still works wherever a display server exists.
    Window(int width, int height, const std::string& title, bool visible = true,
           bool offscreen = false);
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
    bool cursorCaptured() const { return m_captured || m_synthCapture; }
    void mouseDelta(float& dx, float& dy);

    // --- synthetic input (agent autopilot) ---------------------------------
    // A second input source that ORs into the live GLFW reads above, so an MCP
    // agent can "play" the game: in a normal window it combines with human
    // input; in offscreen/headless mode (no GLFW) it is the ONLY source. Held
    // until released (holdSeconds<=0) or auto-released after holdSeconds via
    // tickSyntheticInput. All no-throw, bounds-checked.
    void setSyntheticKey(int glfwKey, bool down, double holdSeconds = 0.0);
    void setSyntheticMouse(int glfwButton, bool down, double holdSeconds = 0.0);
    // Accumulate a one-shot mouse-look delta (consumed by the next mouseDelta).
    void injectMouseLook(float dx, float dy);
    // Make cursorCaptured() report true even with no real capture, so scripts
    // that gate mouse-look on cursor_captured() run under agent control.
    void setSyntheticCapture(bool captured) { m_synthCapture = captured; }
    // Release every synthetic key/button/look/capture (call on Play stop).
    void clearSyntheticInput();
    // Release any synthetic key/button whose hold deadline has passed.
    void tickSyntheticInput(double now);

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

    // --- synthetic input state (agent autopilot, see setSyntheticKey) ---
    bool m_synthKey[512] = {};
    bool m_synthMouse[8] = {};
    float m_synthDX = 0.0f, m_synthDY = 0.0f; // one-shot look delta
    bool m_synthCapture = false;
    // Auto-release deadlines (Window::time() seconds); 0 = held until released.
    double m_synthKeyDeadline[512] = {};
    double m_synthMouseDeadline[8] = {};

    // --- display-less (offscreen) mode ---
    // When m_offscreen, there is no GLFWwindow: m_offCtx owns the GL context and
    // every GLFW-touching method short-circuits (close flag instead of GLFW,
    // stored framebuffer size, steady_clock time, no-op input/swap/poll).
    // raw ptr: the type is absent in a default build, and it's only touched in
    // LIMINAL_HAS_OFFSCREEN paths — [[maybe_unused]] silences the default build.
    [[maybe_unused]] OffscreenContext* m_offCtx = nullptr;
    bool m_offscreen = false;
    bool m_closeFlag = false;
    int m_offW = 0, m_offH = 0;
    long long m_offStartNs = 0; // steady_clock baseline for time()
};

} // namespace liminal
