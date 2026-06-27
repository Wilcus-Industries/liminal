// McpServer implementation. See mcp_server.hpp for the transport + threading
// contract. The one hard rule: anything that reads scene/project state runs
// through marshal() so it executes inside pump() on the main thread.

#include "mcp_server.hpp"

// cpp-httplib is a single heavy header; keep its compile cost to this TU. No
// OpenSSL — plaintext localhost only (CPPHTTPLIB_OPENSSL_SUPPORT stays off).
#include <httplib.h>

#include <liminal/version.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

// stb_image_write's implementation trips -Wextra (missing-field-initializers)
// and -Wdeprecated-declarations (sprintf) — it is vendored third-party code, so
// silence those around just this include while keeping the warnings on our own
// sources in this TU.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace liminal::editor {

namespace {

// Standard base64 encoder (no padding omitted; RFC 4648 alphabet).
std::string base64Encode(const unsigned char* data, std::size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const std::uint32_t n =
            (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(n >> 18) & 0x3f]);
        out.push_back(tbl[(n >> 12) & 0x3f]);
        out.push_back(tbl[(n >> 6) & 0x3f]);
        out.push_back(tbl[n & 0x3f]);
    }
    if (i < len) {
        std::uint32_t n = data[i] << 16;
        const bool two = (i + 1 < len);
        if (two) n |= data[i + 1] << 8;
        out.push_back(tbl[(n >> 18) & 0x3f]);
        out.push_back(tbl[(n >> 12) & 0x3f]);
        out.push_back(two ? tbl[(n >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

constexpr const char* kProtocolVersion = "2024-11-05";
constexpr const char* kServerName = "liminal-editor";
constexpr std::size_t kMaxScriptBytes = 512 * 1024; // clamp read_script output

// MCP tool-result envelope: a single text block carrying the payload (JSON
// dumped to a string, or plain text for read_script).
nlohmann::json textResult(const std::string& text) {
    return {{"content", nlohmann::json::array(
                            {{{"type", "text"}, {"text", text}}})}};
}

// JSON-RPC 2.0 helpers.
nlohmann::json rpcResult(const nlohmann::json& id, const nlohmann::json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}
nlohmann::json rpcError(const nlohmann::json& id, int code,
                        const std::string& message) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"error", {{"code", code}, {"message", message}}}};
}

// The tool schemas advertised by tools/list. Kept here so handleRpc and
// callTool agree on names.
nlohmann::json toolSchemas() {
    auto obj = [](nlohmann::json props, std::vector<std::string> required) {
        nlohmann::json schema = {{"type", "object"}, {"properties", props}};
        if (!required.empty()) schema["required"] = required;
        return schema;
    };
    nlohmann::json none = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    return nlohmann::json::array({
        {{"name", "scene_tree"},
         {"description",
          "List every entity in the current editor scene with its Name and the "
          "names of the components it owns."},
         {"inputSchema", none}},
        {{"name", "get_entity"},
         {"description",
          "Full component JSON for a single entity, addressed by entt id "
          "(numeric string) or by Name."},
         {"inputSchema",
          obj({{"id", {{"type", "string"},
                       {"description", "entt id or entity Name"}}}},
              {"id"})}},
        {{"name", "current_project"},
         {"description",
          "The open project: project file path, title, and current scene path."},
         {"inputSchema", none}},
        {{"name", "list_scripts"},
         {"description", "Enumerate all *.lua files under the project directory "
                         "(relative paths)."},
         {"inputSchema", none}},
        {{"name", "read_script"},
         {"description",
          "Return the contents of a Lua/text file inside the project directory. "
          "Paths outside the project root are rejected; output is size-clamped."},
         {"inputSchema",
          obj({{"path", {{"type", "string"},
                         {"description", "path relative to (or within) the "
                                         "project directory"}}}},
              {"path"})}},
        {{"name", "console_log"},
         {"description",
          "Return the tail of the editor console log as a list of lines."},
         {"inputSchema",
          obj({{"lines", {{"type", "integer"},
                          {"description", "max lines to return (default 200)"}}}},
              {})}},
        {{"name", "play_state"},
         {"description",
          "The editor's play state: mode (\"edit\" or \"play\") and whether it "
          "is paused."},
         {"inputSchema", none}},
        {{"name", "screenshot"},
         {"description",
          "Capture the current low-res viewport framebuffer as a PNG image "
          "(one frame stale)."},
         {"inputSchema", none}},
        {{"name", "play_game"},
         {"description",
          "Enter play-in-editor mode (runs scripts). Returns the resulting "
          "play state."},
         {"inputSchema", none}},
        {{"name", "pause_game"},
         {"description",
          "Pause (or, with paused=false, resume) play-in-editor. Errors if not "
          "currently playing."},
         {"inputSchema",
          obj({{"paused", {{"type", "boolean"},
                           {"description",
                            "true = pause (default), false = resume"}}}},
              {})}},
        {{"name", "stop_game"},
         {"description",
          "Stop play-in-editor and restore the pre-play scene snapshot."},
         {"inputSchema", none}},
        {{"name", "reload_scene"},
         {"description",
          "Reload the current scene from disk, discarding live in-memory edits "
          "(auto-stops Play)."},
         {"inputSchema", none}},
        {{"name", "save_scene"},
         {"description",
          "Save the current scene to disk. With no path, saves to the current "
          "scene path."},
         {"inputSchema",
          obj({{"path", {{"type", "string"},
                         {"description",
                          "destination path (default: current scene path)"}}}},
              {})}},
        {{"name", "set_component"},
         {"description",
          "Add or replace a component on an entity (addressed by entt id or "
          "Name) from a JSON object. On \"Transform\" this edits the transform "
          "(position/rotation/scale); on \"Script\" the data sets the entity's "
          "\"paths\" list of Lua scripts (e.g. {\"paths\":[\"scripts/x.lua\"]}). "
          "Mutation is live/in-memory; call save_scene to persist."},
         {"inputSchema",
          obj({{"id", {{"type", "string"},
                       {"description", "entt id or entity Name"}}},
               {"component", {{"type", "string"},
                              {"description", "component name (e.g. Transform, "
                                              "MeshRenderer, Script)"}}},
               {"data", {{"type", "object"},
                         {"description", "component fields as JSON"}}}},
              {"id", "component"})}},
        {{"name", "remove_component"},
         {"description",
          "Remove a component from an entity (entt id or Name). Mutation is "
          "live/in-memory; call save_scene to persist."},
         {"inputSchema",
          obj({{"id", {{"type", "string"},
                       {"description", "entt id or entity Name"}}},
               {"component", {{"type", "string"},
                              {"description", "component name to remove"}}}},
              {"id", "component"})}},
        {{"name", "create_entity"},
         {"description",
          "Create a new entity in the current scene. A Name component is added "
          "when `name` is non-empty. Mutation is live/in-memory; call "
          "save_scene to persist."},
         {"inputSchema",
          obj({{"name", {{"type", "string"},
                         {"description", "optional Name for the entity"}}}},
              {})}},
        {{"name", "destroy_entity"},
         {"description",
          "Destroy an entity (entt id or Name) from the current scene. Clears "
          "the editor selection if it was the target. Mutation is "
          "live/in-memory; call save_scene to persist."},
         {"inputSchema",
          obj({{"id", {{"type", "string"},
                       {"description", "entt id or entity Name"}}}},
              {"id"})}},
        {{"name", "duplicate_entity"},
         {"description",
          "Duplicate an entity (entt id or Name), copying all of its "
          "components. Mutation is live/in-memory; call save_scene to persist."},
         {"inputSchema",
          obj({{"id", {{"type", "string"},
                       {"description", "entt id or entity Name to duplicate"}}}},
              {"id"})}},
        // --- discovery / introspection --------------------------------------
        {{"name", "list_components"},
         {"description",
          "List every component type the engine can serialize, with each "
          "component's field names, inferred types, and default values. Use "
          "this before set_component so the data payload uses the exact JSON "
          "field names (e.g. MeshRenderer.meshAsset, Transform.rotationEuler)."},
         {"inputSchema", none}},
        {{"name", "list_assets"},
         {"description",
          "Inventory of usable assets: builtin mesh names (builtin:box, ...), "
          "builtin texture names, texture files found under the project, "
          "registered shader packs, and live runtime: keys. Mesh/texture names "
          "here are valid values for MeshRenderer.meshAsset / textureAsset."},
         {"inputSchema", none}},
        {{"name", "list_scenes"},
         {"description",
          "Enumerate all *.lscene files under the project directory (relative "
          "paths). Use a path here with open_scene."},
         {"inputSchema", none}},
        {{"name", "list_shaders"},
         {"description",
          "List the available shader packs and each Camera's current shader "
          "pick. A pack name here is a valid value for Camera.shaderName."},
         {"inputSchema", none}},
        // --- scene management -----------------------------------------------
        {{"name", "open_scene"},
         {"description",
          "Open a different scene file (resolved via the project's asset "
          "search paths; auto-stops Play and discards live edits)."},
         {"inputSchema",
          obj({{"path", {{"type", "string"},
                         {"description", "scene path, e.g. scenes/main.lscene"}}}},
              {"path"})}},
        {{"name", "new_scene"},
         {"description",
          "Replace the current scene with a fresh blank one (not yet saved). "
          "Use save_scene with a path to persist it."},
         {"inputSchema", none}},
        // --- human-in-loop feedback -----------------------------------------
        {{"name", "select_entity"},
         {"description",
          "Select an entity (entt id or Name) in the editor so the human sees "
          "it highlighted in the Inspector and viewport."},
         {"inputSchema",
          obj({{"id", {{"type", "string"},
                       {"description", "entt id or entity Name"}}}},
              {"id"})}},
        {{"name", "get_selection"},
         {"description", "The entity currently selected in the editor, or null."},
         {"inputSchema", none}},
        {{"name", "focus_entity"},
         {"description",
          "Point the editor camera at an entity (entt id or Name). Pair with "
          "screenshot to see your work. Edit mode only."},
         {"inputSchema",
          obj({{"id", {{"type", "string"},
                       {"description", "entt id or entity Name"}}}},
              {"id"})}},
        // --- verify / query + build -----------------------------------------
        {{"name", "raycast"},
         {"description",
          "Cast a ray against the scene's Collider/mesh AABBs. Returns the "
          "nearest hit { entity, point, normal, distance } or null."},
         {"inputSchema",
          obj({{"origin", {{"type", "array"},
                           {"description", "[x,y,z] ray origin"}}},
               {"dir", {{"type", "array"},
                        {"description", "[x,y,z] ray direction"}}},
               {"maxDist", {{"type", "number"},
                            {"description",
                             "max distance (<=0 = unbounded, default 0)"}}}},
              {"origin", "dir"})}},
        {{"name", "validate_scene"},
         {"description",
          "Check the current scene for problems: unresolved MeshRenderer "
          "meshAsset/textureAsset, missing Script files, and a primary-camera "
          "count other than one. Returns { ok, issues, primaryCameraCount }."},
         {"inputSchema", none}},
        {{"name", "build_game"},
         {"description",
          "Build the standalone game (packs a .pak + a player executable). "
          "With no path, builds to a default location under the project dir. "
          "The build is synchronous and may exceed the response window — if so "
          "you get a timeout; poll console_log for the build result."},
         {"inputSchema",
          obj({{"path", {{"type", "string"},
                         {"description",
                          "output executable path (default: under the project "
                          "dir)"}}}},
              {})}},
    });
}

} // namespace

std::string mcpEncodePngBase64(const std::vector<unsigned char>& rgba, int w,
                               int h) {
    if (w <= 0 || h <= 0 ||
        rgba.size() < static_cast<std::size_t>(w) * h * 4)
        return {};

    // The FBO is bottom-up; PNG expects top-down. Flip rows into a scratch
    // buffer before encoding.
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    std::vector<unsigned char> flipped(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = rgba.data() + stride * (h - 1 - y);
        std::copy(src, src + stride, flipped.data() + stride * y);
    }

    std::vector<unsigned char> png;
    auto sink = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* bytes = static_cast<const unsigned char*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    if (!stbi_write_png_to_func(sink, &png, w, h, 4, flipped.data(),
                                static_cast<int>(stride)))
        return {};
    return base64Encode(png.data(), png.size());
}

McpServer::McpServer(McpProvider provider) : m_provider(std::move(provider)) {}

McpServer::~McpServer() {
    m_running = false;
    if (m_svr) m_svr->stop();
    if (m_thread.joinable()) m_thread.join();
    // Anyone still blocked in marshal() will time out and take the fallback.
}

int McpServer::start(int preferred) {
    if (m_running) return m_port;

    m_svr = std::make_unique<httplib::Server>();

    // Single POST endpoint — the Streamable HTTP transport. One JSON-RPC message
    // per request; notifications (no "id") get an empty 202, requests get their
    // response as application/json.
    m_svr->Post("/mcp", [this](const httplib::Request& req,
                               httplib::Response& res) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            res.set_content(rpcError(nullptr, -32700, "parse error").dump(),
                            "application/json");
            return;
        }
        nlohmann::json reply = handleRpc(body);
        if (reply.is_null()) {
            // Notification: no response body.
            res.status = 202;
            res.set_content("", "application/json");
            return;
        }
        res.set_content(reply.dump(), "application/json");
    });

    // Find a free port: preferred first, then a few sequential fallbacks.
    int chosen = 0;
    for (int p = preferred; p < preferred + 16; ++p) {
        if (m_svr->bind_to_port("127.0.0.1", p)) {
            chosen = p;
            break;
        }
    }
    if (chosen == 0) {
        logLine("[mcp] could not bind any port in [" + std::to_string(preferred) +
                ", " + std::to_string(preferred + 16) + ")");
        m_svr.reset();
        return 0;
    }

    m_port = chosen;
    m_running = true;
    m_thread = std::thread([this] { m_svr->listen_after_bind(); });
    return chosen;
}

void McpServer::pump() {
    // Drain under the lock, then run outside it (tasks may be slow; we don't
    // want http threads blocked on enqueue while a task runs).
    std::queue<Task> local;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::swap(local, m_queue);
    }
    while (!local.empty()) {
        local.front().run();
        local.pop();
    }
}

nlohmann::json McpServer::marshal(std::function<nlohmann::json()> fn,
                                  const nlohmann::json& onTimeout) {
    if (!m_running) return onTimeout;

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    std::future<nlohmann::json> fut = promise->get_future();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push(Task{[fn = std::move(fn), promise]() mutable {
            try {
                promise->set_value(fn());
            } catch (const std::exception& e) {
                promise->set_value(nlohmann::json{{"error", e.what()}});
            }
        }});
    }
    // Block the http thread on the main thread fulfilling the promise. A timeout
    // protects against a stalled / closing main loop.
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        return onTimeout;
    return fut.get();
}

nlohmann::json McpServer::marshalText(std::function<nlohmann::json()> fn,
                                      int indent) {
    static const nlohmann::json timeoutResult =
        textResult("{\"error\":\"editor did not respond (main thread busy)\"}");
    nlohmann::json out = marshal(std::move(fn), timeoutResult);
    // The timeout envelope is already an MCP content block; pass it through.
    if (out.is_object() && out.contains("content")) return out;
    return textResult(out.dump(indent));
}

nlohmann::json McpServer::handleRpc(const nlohmann::json& req) {
    // Batch requests are not used by Claude Code's http transport here; handle
    // the single-object shape.
    if (!req.is_object()) return rpcError(nullptr, -32600, "invalid request");

    const std::string method = req.value("method", std::string{});
    const nlohmann::json id = req.contains("id") ? req["id"] : nlohmann::json(nullptr);
    const bool isNotification = !req.contains("id");

    if (method == "initialize") {
        nlohmann::json result = {
            {"protocolVersion", kProtocolVersion},
            {"capabilities", {{"tools", nlohmann::json::object()}}},
            {"serverInfo", {{"name", kServerName}, {"version", liminal::kVersionString}}}};
        return rpcResult(id, result);
    }
    if (method == "notifications/initialized" || method == "notifications/cancelled") {
        return nullptr; // no-op notification
    }
    if (method == "tools/list") {
        return rpcResult(id, {{"tools", toolSchemas()}});
    }
    if (method == "tools/call") {
        const nlohmann::json params = req.value("params", nlohmann::json::object());
        const std::string name = params.value("name", std::string{});
        const nlohmann::json args =
            params.value("arguments", nlohmann::json::object());
        return rpcResult(id, callTool(name, args));
    }

    if (isNotification) return nullptr;
    return rpcError(id, -32601, "method not found: " + method);
}

nlohmann::json McpServer::callTool(const std::string& name,
                                   const nlohmann::json& args) {
    const nlohmann::json timeoutResult =
        textResult("{\"error\":\"editor did not respond (main thread busy)\"}");

    if (name == "scene_tree") {
        return marshalText(
            [this]() -> nlohmann::json {
                return m_provider.sceneTree ? m_provider.sceneTree()
                                            : nlohmann::json::object();
            },
            2);
    }

    if (name == "get_entity") {
        const std::string key = args.value("id", std::string{});
        nlohmann::json ent = marshal(
            [this, key]() -> nlohmann::json {
                return m_provider.getEntity ? m_provider.getEntity(key)
                                            : nlohmann::json(nullptr);
            },
            timeoutResult);
        if (ent.is_object() && ent.contains("content")) return ent; // timeout
        if (ent.is_null())
            return textResult("{\"error\":\"no entity matching \\\"" + key + "\\\"\"}");
        return textResult(ent.dump(2));
    }

    if (name == "current_project") {
        return marshalText(
            [this]() -> nlohmann::json {
                return m_provider.currentProject ? m_provider.currentProject()
                                                 : nlohmann::json::object();
            },
            2);
    }

    if (name == "list_scripts") {
        // The directory walk itself is filesystem-only, but the root comes from
        // the editor — fetch it on the main thread, then scan here.
        std::string root = marshal(
                               [this]() -> nlohmann::json {
                                   return m_provider.projectDir
                                              ? m_provider.projectDir()
                                              : std::string{};
                               },
                               nlohmann::json(std::string{}))
                               .get<std::string>();
        if (root.empty())
            return textResult("{\"error\":\"no project open\"}");
        nlohmann::json scripts = nlohmann::json::array();
        std::error_code ec;
        const fs::path base = fs::path(root);
        for (auto it = fs::recursive_directory_iterator(
                 base, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() == ".lua") {
                scripts.push_back(
                    fs::relative(it->path(), base, ec).generic_string());
            }
        }
        std::sort(scripts.begin(), scripts.end());
        return textResult(scripts.dump(2));
    }

    if (name == "read_script") {
        const std::string rel = args.value("path", std::string{});
        std::string root = marshal(
                               [this]() -> nlohmann::json {
                                   return m_provider.projectDir
                                              ? m_provider.projectDir()
                                              : std::string{};
                               },
                               nlohmann::json(std::string{}))
                               .get<std::string>();
        if (root.empty())
            return textResult("{\"error\":\"no project open\"}");

        // Sandbox: resolve against the project root and verify the canonical
        // path stays within it (defeats ../ escapes and symlink tricks).
        std::error_code ec;
        const fs::path base = fs::weakly_canonical(fs::path(root), ec);
        fs::path target = fs::path(rel);
        if (target.is_relative()) target = base / target;
        const fs::path canon = fs::weakly_canonical(target, ec);
        const std::string baseStr = base.generic_string();
        const std::string canonStr = canon.generic_string();
        if (canonStr.rfind(baseStr, 0) != 0)
            return textResult("{\"error\":\"path escapes project directory\"}");
        if (!fs::is_regular_file(canon, ec))
            return textResult("{\"error\":\"not a file\"}");

        std::ifstream in(canon, std::ios::binary);
        if (!in)
            return textResult("{\"error\":\"cannot open file\"}");
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string contents = ss.str();
        if (contents.size() > kMaxScriptBytes) {
            contents.resize(kMaxScriptBytes);
            contents += "\n... [truncated]";
        }
        return textResult(contents); // plain text body
    }

    if (name == "console_log") {
        const int lines = args.value("lines", 200);
        return marshalText(
            [this, lines]() -> nlohmann::json {
                return m_provider.consoleLog
                           ? m_provider.consoleLog(lines)
                           : nlohmann::json{{"lines", nlohmann::json::array()}};
            },
            2);
    }

    if (name == "play_state") {
        return marshalText(
            [this]() -> nlohmann::json {
                return m_provider.playState ? m_provider.playState()
                                            : nlohmann::json::object();
            },
            2);
    }

    if (name == "screenshot") {
        nlohmann::json shot = marshal(
            [this]() -> nlohmann::json {
                return m_provider.screenshot
                           ? m_provider.screenshot()
                           : nlohmann::json{{"error", "screenshot unavailable"}};
            },
            timeoutResult);
        if (shot.is_object() && shot.contains("content")) return shot; // timeout
        if (!shot.is_object() || shot.contains("error"))
            return textResult(
                shot.is_object() ? shot.dump() : nlohmann::json{shot}.dump());
        // Return an MCP image content block instead of text.
        return {{"content",
                 nlohmann::json::array({{{"type", "image"},
                                         {"data", shot.value("base64", "")},
                                         {"mimeType",
                                          shot.value("mimeType",
                                                     "image/png")}}})}};
    }

    if (name == "play_game") {
        return marshalText([this]() -> nlohmann::json {
            return m_provider.control
                       ? m_provider.control("play")
                       : nlohmann::json{{"error", "control unavailable"}};
        });
    }

    if (name == "stop_game") {
        return marshalText([this]() -> nlohmann::json {
            return m_provider.control
                       ? m_provider.control("stop")
                       : nlohmann::json{{"error", "control unavailable"}};
        });
    }

    if (name == "pause_game") {
        const bool paused = args.value("paused", true);
        return marshalText([this, paused]() -> nlohmann::json {
            if (!m_provider.control)
                return nlohmann::json{{"error", "control unavailable"}};
            return m_provider.control(paused ? "pause" : "resume");
        });
    }

    if (name == "reload_scene") {
        return marshalText([this]() -> nlohmann::json {
            return m_provider.reloadScene
                       ? m_provider.reloadScene()
                       : nlohmann::json{{"error", "reload unavailable"}};
        });
    }

    if (name == "save_scene") {
        const std::string path = args.value("path", std::string{});
        return marshalText([this, path]() -> nlohmann::json {
            return m_provider.saveScene
                       ? m_provider.saveScene(path)
                       : nlohmann::json{{"error", "save unavailable"}};
        });
    }

    if (name == "set_component") {
        const std::string id = args.value("id", std::string{});
        const std::string comp = args.value("component", std::string{});
        if (id.empty() || comp.empty())
            return textResult(
                "{\"error\":\"set_component requires 'id' and 'component'\"}");
        const nlohmann::json data = args.value("data", nlohmann::json::object());
        return marshalText([this, id, comp, data]() -> nlohmann::json {
            return m_provider.setComponent
                       ? m_provider.setComponent(id, comp, data)
                       : nlohmann::json{{"error", "setComponent unavailable"}};
        });
    }

    if (name == "remove_component") {
        const std::string id = args.value("id", std::string{});
        const std::string comp = args.value("component", std::string{});
        if (id.empty() || comp.empty())
            return textResult(
                "{\"error\":\"remove_component requires 'id' and 'component'\"}");
        return marshalText([this, id, comp]() -> nlohmann::json {
            return m_provider.removeComponent
                       ? m_provider.removeComponent(id, comp)
                       : nlohmann::json{{"error", "removeComponent unavailable"}};
        });
    }

    if (name == "create_entity") {
        const std::string entName = args.value("name", std::string{});
        return marshalText([this, entName]() -> nlohmann::json {
            return m_provider.createEntity
                       ? m_provider.createEntity(entName)
                       : nlohmann::json{{"error", "createEntity unavailable"}};
        });
    }

    if (name == "destroy_entity") {
        const std::string id = args.value("id", std::string{});
        if (id.empty())
            return textResult("{\"error\":\"destroy_entity requires 'id'\"}");
        return marshalText([this, id]() -> nlohmann::json {
            return m_provider.destroyEntity
                       ? m_provider.destroyEntity(id)
                       : nlohmann::json{{"error", "destroyEntity unavailable"}};
        });
    }

    if (name == "duplicate_entity") {
        const std::string id = args.value("id", std::string{});
        if (id.empty())
            return textResult("{\"error\":\"duplicate_entity requires 'id'\"}");
        return marshalText([this, id]() -> nlohmann::json {
            return m_provider.duplicateEntity
                       ? m_provider.duplicateEntity(id)
                       : nlohmann::json{{"error", "duplicateEntity unavailable"}};
        });
    }

    if (name == "list_components") {
        return marshalText(
            [this]() -> nlohmann::json {
                return m_provider.listComponents
                           ? m_provider.listComponents()
                           : nlohmann::json{{"components", nlohmann::json::array()}};
            },
            2);
    }

    if (name == "list_assets") {
        return marshalText(
            [this]() -> nlohmann::json {
                return m_provider.listAssets ? m_provider.listAssets()
                                             : nlohmann::json::object();
            },
            2);
    }

    if (name == "list_scenes") {
        // Filesystem-only walk, same shape as list_scripts: pull the root on the
        // main thread, then scan here for *.lscene.
        std::string root = marshal(
                               [this]() -> nlohmann::json {
                                   return m_provider.projectDir
                                              ? m_provider.projectDir()
                                              : std::string{};
                               },
                               nlohmann::json(std::string{}))
                               .get<std::string>();
        if (root.empty())
            return textResult("{\"error\":\"no project open\"}");
        nlohmann::json scenes = nlohmann::json::array();
        std::error_code ec;
        const fs::path base = fs::path(root);
        for (auto it = fs::recursive_directory_iterator(
                 base, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() == ".lscene")
                scenes.push_back(
                    fs::relative(it->path(), base, ec).generic_string());
        }
        std::sort(scenes.begin(), scenes.end());
        return textResult(scenes.dump(2));
    }

    if (name == "list_shaders") {
        return marshalText(
            [this]() -> nlohmann::json {
                return m_provider.listShaders ? m_provider.listShaders()
                                              : nlohmann::json::object();
            },
            2);
    }

    if (name == "open_scene") {
        const std::string path = args.value("path", std::string{});
        if (path.empty())
            return textResult("{\"error\":\"open_scene requires 'path'\"}");
        return marshalText([this, path]() -> nlohmann::json {
            return m_provider.openScene
                       ? m_provider.openScene(path)
                       : nlohmann::json{{"error", "openScene unavailable"}};
        });
    }

    if (name == "new_scene") {
        return marshalText([this]() -> nlohmann::json {
            return m_provider.newScene
                       ? m_provider.newScene()
                       : nlohmann::json{{"error", "newScene unavailable"}};
        });
    }

    if (name == "select_entity") {
        const std::string id = args.value("id", std::string{});
        if (id.empty())
            return textResult("{\"error\":\"select_entity requires 'id'\"}");
        return marshalText([this, id]() -> nlohmann::json {
            return m_provider.selectEntity
                       ? m_provider.selectEntity(id)
                       : nlohmann::json{{"error", "selectEntity unavailable"}};
        });
    }

    if (name == "get_selection") {
        return marshalText(
            [this]() -> nlohmann::json {
                return m_provider.getSelection ? m_provider.getSelection()
                                               : nlohmann::json(nullptr);
            },
            2);
    }

    if (name == "focus_entity") {
        const std::string id = args.value("id", std::string{});
        if (id.empty())
            return textResult("{\"error\":\"focus_entity requires 'id'\"}");
        return marshalText([this, id]() -> nlohmann::json {
            return m_provider.focusEntity
                       ? m_provider.focusEntity(id)
                       : nlohmann::json{{"error", "focusEntity unavailable"}};
        });
    }

    if (name == "raycast") {
        const nlohmann::json origin = args.value("origin", nlohmann::json::array());
        const nlohmann::json dir = args.value("dir", nlohmann::json::array());
        const double maxDist = args.value("maxDist", 0.0);
        if (!origin.is_array() || origin.size() != 3 || !dir.is_array() ||
            dir.size() != 3)
            return textResult(
                "{\"error\":\"raycast requires 'origin' and 'dir' as [x,y,z]\"}");
        return marshalText(
            [this, origin, dir, maxDist]() -> nlohmann::json {
                return m_provider.raycast
                           ? m_provider.raycast(origin, dir, maxDist)
                           : nlohmann::json(nullptr);
            },
            2);
    }

    if (name == "validate_scene") {
        return marshalText(
            [this]() -> nlohmann::json {
                return m_provider.validateScene ? m_provider.validateScene()
                                                : nlohmann::json::object();
            },
            2);
    }

    if (name == "build_game") {
        const std::string path = args.value("path", std::string{});
        return marshalText([this, path]() -> nlohmann::json {
            return m_provider.buildGame
                       ? m_provider.buildGame(path)
                       : nlohmann::json{{"error", "buildGame unavailable"}};
        });
    }

    return textResult("{\"error\":\"unknown tool: " + name + "\"}");
}

} // namespace liminal::editor
