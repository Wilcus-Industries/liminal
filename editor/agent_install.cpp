// agent_install.cpp — install the agent bootstrap skill. See agent_install.hpp.
#include "agent_install.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include <liminal/core/platform.hpp>

#include "resource_paths.hpp"

namespace fs = std::filesystem;

namespace liminal::editor {
namespace {

#if defined(LIMINAL_EDITOR_BOOTSTRAP_SKILL)
constexpr const char* kBakedBootstrap = LIMINAL_EDITOR_BOOTSTRAP_SKILL;
#else
constexpr const char* kBakedBootstrap = "";
#endif

// Replace every occurrence of `from` with `to` in-place.
void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;
         pos += to.size())
        s.replace(pos, from.size(), to);
}

// Strip a leading YAML frontmatter block ("---\n ... \n---\n") for agents that
// don't use Claude's skill-with-frontmatter format.
std::string stripFrontmatter(const std::string& s) {
    if (s.rfind("---\n", 0) != 0) return s;
    const std::size_t end = s.find("\n---\n", 4);
    if (end == std::string::npos) return s;
    return s.substr(end + 5);
}

// Read the baked bootstrap source (relocatable: bundle copy else baked path).
std::string readBootstrapSource() {
    const std::string src =
        resolveResource("skills/liminal/SKILL.md", kBakedBootstrap);
    if (src.empty()) return {};
    std::ifstream in(src, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Destination file for each target under $HOME, plus whether to keep frontmatter.
struct Dest {
    fs::path path;
    bool keepFrontmatter = false;
};

Dest destFor(AgentTarget target, const fs::path& home) {
    switch (target) {
    case AgentTarget::Claude:
        return {home / ".claude" / "skills" / "liminal" / "SKILL.md", true};
    case AgentTarget::Codex:
        return {home / ".codex" / "liminal-bootstrap.md", false};
    case AgentTarget::Generic:
    default:
        return {home / ".config" / "liminal" / "agent-bootstrap.md", false};
    }
}

// Merge a user-scope `liminal` HTTP MCP entry into ~/.claude.json, preserving
// every other key. Refuses to clobber an existing-but-unparseable config (it is
// the user's whole Claude Code config — losing it would be destructive).
void registerUserScopeMcp(const fs::path& home, int port, InstallResult& r) {
    const fs::path cfg = home / ".claude.json";
    nlohmann::json doc = nlohmann::json::object();
    std::error_code ec;
    if (fs::exists(cfg, ec)) {
        std::ifstream in(cfg);
        if (in) {
            try {
                in >> doc;
            } catch (const std::exception&) {
                r.message += " (skipped ~/.claude.json: unparseable, not clobbered)";
                return;
            }
            if (!doc.is_object()) {
                r.message += " (skipped ~/.claude.json: not a JSON object)";
                return;
            }
        }
    }
    if (!doc.contains("mcpServers") || !doc["mcpServers"].is_object())
        doc["mcpServers"] = nlohmann::json::object();
    doc["mcpServers"]["liminal"] = {
        {"type", "http"},
        {"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"}};

    std::ofstream out(cfg);
    if (!out) {
        r.message += " (could not write ~/.claude.json)";
        return;
    }
    out << doc.dump(2) << '\n';
    r.message += " (registered user-scope MCP -> port " + std::to_string(port) + ")";
}

} // namespace

const char* agentTargetName(AgentTarget t) {
    switch (t) {
    case AgentTarget::Claude:  return "claude";
    case AgentTarget::Codex:   return "codex";
    case AgentTarget::Generic: return "generic";
    }
    return "?";
}

bool parseAgentTargets(const std::string& spec, std::vector<AgentTarget>& out) {
    auto add = [&](AgentTarget t) {
        for (auto e : out)
            if (e == t) return; // dedupe
        out.push_back(t);
    };
    std::stringstream ss(spec);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok == "all") {
            add(AgentTarget::Claude);
            add(AgentTarget::Codex);
            add(AgentTarget::Generic);
        } else if (tok == "claude" || tok.empty()) {
            add(AgentTarget::Claude);
        } else if (tok == "codex") {
            add(AgentTarget::Codex);
        } else if (tok == "generic") {
            add(AgentTarget::Generic);
        } else {
            return false;
        }
    }
    if (out.empty()) add(AgentTarget::Claude); // empty spec -> claude
    return true;
}

std::string bootstrapSkillText(int mcpPort) {
    if (mcpPort <= 0) mcpPort = kDefaultMcpPort;
    std::string text = readBootstrapSource();
    if (text.empty()) return {};
    std::string exe = liminal::selfExePath();
    if (exe.empty()) exe = "liminal-editor";
    replaceAll(text, "{{LIMINAL_EDITOR}}", exe);
    replaceAll(text, "{{MCP_PORT}}", std::to_string(mcpPort));
    return text;
}

InstallResult installAgentSkill(AgentTarget target, int mcpPort, bool force) {
    InstallResult r;
    if (mcpPort <= 0) mcpPort = kDefaultMcpPort;

    const fs::path home = liminal::userHomeDir();
    if (home.empty()) {
        r.message = "no home directory ($HOME)";
        return r;
    }

    std::string text = bootstrapSkillText(mcpPort);
    if (text.empty()) {
        r.message = "bootstrap skill source not found";
        return r;
    }

    const Dest dest = destFor(target, home);
    r.path = dest.path.string();
    if (!dest.keepFrontmatter) text = stripFrontmatter(text);

    std::error_code ec;
    const bool exists = fs::exists(dest.path, ec);
    if (exists && !force) {
        // Never clobber a customized doc; still refresh the user-scope MCP entry
        // for Claude so a re-installed/moved editor keeps the URL correct.
        r.ok = true;
        r.message = "already present (not overwritten)";
        if (target == AgentTarget::Claude) registerUserScopeMcp(home, mcpPort, r);
        return r;
    }

    fs::create_directories(dest.path.parent_path(), ec);
    if (ec) {
        r.message = "could not create " + dest.path.parent_path().string();
        return r;
    }
    std::ofstream out(dest.path, std::ios::binary);
    if (!out) {
        r.message = "could not write " + r.path;
        return r;
    }
    out << text;
    out.close();
    r.ok = true;
    r.wrote = true;
    r.message = force && exists ? "overwritten" : "installed";
    if (target == AgentTarget::Claude) registerUserScopeMcp(home, mcpPort, r);
    return r;
}

} // namespace liminal::editor
