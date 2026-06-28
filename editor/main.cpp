// liminal-editor entry point.
//   liminal-editor                          -> landing screen (project chooser)
//   liminal-editor --project <path>         -> opens project.ljson (or its dir)
//   liminal-editor --empty                  -> no project, blank editor scene
//   liminal-editor --headless --project <p> -> no GUI: engine + MCP server only,
//                                              for an external agent to drive.
//                                              Auto-scaffolds an empty/new dir.
//   --display <auto|offscreen|glfw>         -> headless context backend:
//        auto       (default) display-less offscreen if one was compiled in,
//                   else a hidden GLFW window;
//        offscreen  force the display-less EGL/OSMesa backend;
//        glfw       force a hidden GLFW window (needs a display server).
#include "editor_app.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

// A display-less offscreen backend is compiled in iff one of these is defined
// (PUBLIC compile defs from the liminal lib, propagated through the link).
#if defined(LIMINAL_HEADLESS_EGL) || defined(LIMINAL_HEADLESS_OSMESA)
#define LIMINAL_OFFSCREEN_AVAILABLE 1
#endif

namespace {
// Set by SIGINT/SIGTERM so the headless loop exits cleanly. The handler only
// flips the GLFW close flag (requestQuit), which the loop polls.
liminal::editor::EditorApp* g_app = nullptr;

void onSignal(int) {
    if (g_app) g_app->requestQuit();
}
} // namespace

int main(int argc, char** argv) {
    std::string project; // empty -> landing screen
    bool empty = false;
    bool headless = false;
    std::string display = "auto"; // auto | offscreen | glfw
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project = argv[++i];
        } else if (std::strcmp(argv[i], "--empty") == 0) {
            empty = true;
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            display = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: liminal-editor [--project <project.ljson|dir>] "
                "[--empty] [--headless] [--display auto|offscreen|glfw]\n"
                "  --headless  run engine + MCP server with no GUI; "
                "requires --project (an empty dir is scaffolded).\n"
                "  --display   headless context backend: auto (default) prefers "
                "the display-less\n"
                "              offscreen backend if compiled in, else a hidden "
                "window; offscreen\n"
                "              forces EGL/OSMesa (no display server); glfw forces "
                "a hidden window.\n");
            return 0;
        }
    }

    // The landing chooser is GUI-only, so headless must be handed a project dir.
    if (headless && project.empty()) {
        std::fprintf(stderr,
                     "liminal-editor: --headless requires --project <dir>\n");
        return 2;
    }

    // Resolve whether to use the display-less offscreen context.
#if defined(LIMINAL_OFFSCREEN_AVAILABLE)
    constexpr bool kOffscreenCompiled = true;
#else
    constexpr bool kOffscreenCompiled = false;
#endif
    bool offscreen = false;
    if (headless) {
        if (display == "glfw") {
            offscreen = false;
        } else if (display == "offscreen") {
            offscreen = true; // Window falls back to a hidden window if absent
            if (!kOffscreenCompiled)
                std::fprintf(stderr,
                             "liminal-editor: --display offscreen but no "
                             "offscreen backend was compiled in "
                             "(reconfigure with -DLIMINAL_HEADLESS_OFFSCREEN=ON); "
                             "falling back to a hidden window.\n");
        } else { // auto
            offscreen = kOffscreenCompiled;
        }
    }

    liminal::editor::EditorApp app(project, empty, headless, offscreen);
    if (headless) {
        g_app = &app;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        app.runHeadless();
    } else {
        app.run();
    }
    return 0;
}
