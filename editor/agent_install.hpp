#pragma once
// Agent bootstrap-skill installation.
//
// A fresh `claude` (or other agent) opened in an empty directory has no way to
// discover liminal: the per-project liminal-lua skill + .mcp.json are only
// written once the editor already has a project open (chicken-and-egg). This
// module installs a small, agent-neutral *launcher* doc
// (editor/skills/liminal/SKILL.md, baked via LIMINAL_EDITOR_BOOTSTRAP_SKILL)
// into an agent's global convention directory under $HOME, so any future
// session knows how to launch + connect to the headless editor. The launch +
// curl commands in the doc are stamped with the running editor's absolute path
// and the pinned MCP port.

#include <string>
#include <vector>

namespace liminal::editor {

// Default MCP port the headless editor prefers (mirrors McpServer::start's
// preferred arg). Used as the pinned port when none is supplied.
inline constexpr int kDefaultMcpPort = 7717;

// Which agent's convention directory to install into.
enum class AgentTarget {
    Claude,  // ~/.claude/skills/liminal/SKILL.md (+ user-scope ~/.claude.json)
    Codex,   // ~/.codex/liminal-bootstrap.md (plain, no frontmatter)
    Generic, // ~/.config/liminal/agent-bootstrap.md (plain, no frontmatter)
};

struct InstallResult {
    bool ok = false;        // resolved cleanly (written, or already present)
    bool wrote = false;     // a file was actually (over)written this call
    std::string path;       // destination path (attempted)
    std::string message;    // human-readable detail (logged by callers)
};

// Install the bootstrap doc for `target`. `mcpPort` (<=0 -> kDefaultMcpPort) is
// stamped into the doc and, for Claude, into the user-scope ~/.claude.json
// `liminal` MCP entry. Idempotent: never clobbers an existing skill file unless
// `force`. The ~/.claude.json merge always refreshes the `liminal` entry but
// preserves every other key (and refuses to clobber an unparseable config).
InstallResult installAgentSkill(AgentTarget target, int mcpPort = kDefaultMcpPort,
                                bool force = false);

// Parse a --install-skill value: "claude" | "codex" | "generic" | "all" (or a
// comma-separated list). Appends to `out`. Returns false on an unknown token.
bool parseAgentTargets(const std::string& spec, std::vector<AgentTarget>& out);

// Human-readable name for logging / CLI output.
const char* agentTargetName(AgentTarget t);

// The baked bootstrap doc with {{LIMINAL_EDITOR}} / {{MCP_PORT}} substituted —
// frontmatter included. Backs `--agent-help` / `--print-skill`. Empty string if
// the baked source can't be found.
std::string bootstrapSkillText(int mcpPort = kDefaultMcpPort);

} // namespace liminal::editor
