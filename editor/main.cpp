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
//   --mcp-port <n>                          -> pin the MCP server to port n
//                                              (deterministic bind; no probe).
//   --install-skill[=claude|codex|generic|all] [--force]
//                                           -> install the agent bootstrap skill
//                                              into the agent's ~/ convention dir
//                                              and exit (no editor launch).
//   --agent-help (--print-skill)            -> print the bootstrap skill (launch
//                                              + connect instructions) and exit.
#include "agent_install.hpp"
#include "editor_app.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
    int mcpPort = 0;              // >0 -> pin the MCP server to this exact port
    bool installSkill = false;    // --install-skill: install + exit
    std::string installSpec = "claude"; // targets for --install-skill
    bool force = false;           // --force: overwrite an existing skill doc
    bool agentHelp = false;       // --agent-help / --print-skill: print + exit
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project = argv[++i];
        } else if (std::strcmp(argv[i], "--empty") == 0) {
            empty = true;
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            display = argv[++i];
        } else if (std::strcmp(argv[i], "--mcp-port") == 0 && i + 1 < argc) {
            mcpPort = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--install-skill") == 0) {
            installSkill = true; // default target (claude)
        } else if (std::strncmp(argv[i], "--install-skill=", 16) == 0) {
            installSkill = true;
            installSpec = argv[i] + 16;
        } else if (std::strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (std::strcmp(argv[i], "--agent-help") == 0 ||
                   std::strcmp(argv[i], "--print-skill") == 0) {
            agentHelp = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: liminal-editor [--project <project.ljson|dir>] "
                "[--empty] [--headless] [--display auto|offscreen|glfw]\n"
                "                      [--mcp-port <n>] "
                "[--install-skill[=claude|codex|generic|all] [--force]] "
                "[--agent-help]\n"
                "  --headless      run engine + MCP server with no GUI; "
                "requires --project (an empty dir is scaffolded).\n"
                "  --display       headless context backend: auto (default) "
                "prefers the display-less\n"
                "                  offscreen backend if compiled in, else a "
                "hidden window; offscreen\n"
                "                  forces EGL/OSMesa (no display server); glfw "
                "forces a hidden window.\n"
                "  --mcp-port      pin the in-editor MCP server to this exact "
                "port (default: probe from 7717).\n"
                "  --install-skill install the agent bootstrap skill into an "
                "agent's ~/ convention dir and exit\n"
                "                  (default claude). --force overwrites an "
                "existing skill doc.\n"
                "  --agent-help    print the agent bootstrap skill (how to "
                "launch + connect to the editor) and exit.\n");
            return 0;
        }
    }

    // --agent-help / --print-skill: dump the bootstrap doc (with the running
    // editor's path + pinned port stamped in) to stdout and exit.
    if (agentHelp) {
        const std::string doc = liminal::editor::bootstrapSkillText(mcpPort);
        if (doc.empty()) {
            std::fprintf(stderr,
                         "liminal-editor: bootstrap skill source not found\n");
            return 1;
        }
        std::fputs(doc.c_str(), stdout);
        return 0;
    }

    // --install-skill: install into each requested agent's convention dir + exit.
    if (installSkill) {
        std::vector<liminal::editor::AgentTarget> targets;
        if (!liminal::editor::parseAgentTargets(installSpec, targets)) {
            std::fprintf(stderr,
                         "liminal-editor: unknown --install-skill target in "
                         "'%s' (want claude|codex|generic|all)\n",
                         installSpec.c_str());
            return 2;
        }
        int rc = 0;
        for (auto t : targets) {
            const auto r = liminal::editor::installAgentSkill(t, mcpPort, force);
            std::printf("[install-skill] %s: %s -> %s%s\n",
                        liminal::editor::agentTargetName(t),
                        r.ok ? (r.wrote ? "wrote" : "ok") : "FAILED",
                        r.path.empty() ? "(no path)" : r.path.c_str(),
                        r.message.empty() ? "" : ("  " + r.message).c_str());
            if (!r.ok) rc = 1;
        }
        return rc;
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

    liminal::editor::EditorApp app(project, empty, headless, offscreen, mcpPort);
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
