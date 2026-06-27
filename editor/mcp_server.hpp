#pragma once
// McpServer: a Model Context Protocol server hosted inside liminal-editor so
// that Claude Code (running in the editor's Terminal panel) can introspect the
// live editor state — the current scene, the open project, and the project's
// Lua scripts.
//
// Transport: MCP "Streamable HTTP" over a single POST endpoint on
// 127.0.0.1 (localhost-only, no TLS — the editor is the long-lived process and
// the client lives in the same machine). Each POST /mcp carries one JSON-RPC
// 2.0 request and gets one JSON-RPC 2.0 response back as application/json. SSE
// streaming is intentionally skipped: every tool here is request/response, so a
// plain JSON body per POST is sufficient.
//
// Threading model (mirrors the audio / terminal discipline of keeping worker
// threads off shared mutable state): cpp-httplib runs the server on its own
// thread(s). entt / Scene / the EditorApp's project fields are NOT thread-safe
// and live on the main (ImGui) thread. So a tool NEVER touches scene state on
// the http thread — instead it packages its scene-reading work into a
// std::function, hands it to a mutex-guarded queue with a std::promise, and
// blocks on the matching std::future (with a timeout). The main thread calls
// pump() once per frame to drain the queue and run each task ON the main thread,
// fulfilling the promise. This is the "game thread owns the data" invariant: the
// dispatch lambda runs only inside pump(), so it can safely read m_scene etc.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace httplib { class Server; }

namespace liminal::editor {

// Read-only view of editor state, populated on the main thread by a tool's
// dispatch lambda. The McpServer never reaches into EditorApp directly; the
// editor supplies a Provider whose getters all run inside pump() (main thread),
// so reading m_scene/m_projectFile from them is safe.
struct McpProvider {
    // Full scene tree as JSON: { entities: [ { id, name, components:[...] } ] }.
    std::function<nlohmann::json()> sceneTree;
    // One entity by entt id (>=0) or by Name. Returns null json if not found.
    std::function<nlohmann::json(const std::string& idOrName)> getEntity;
    // { projectFile, title, scenePath } — strings as the editor knows them.
    std::function<nlohmann::json()> currentProject;
    // Absolute project directory ("" if no project) — used for script listing
    // and the read_script sandbox root. Read on the main thread.
    std::function<std::string()> projectDir;
    // Tail of the editor console: { "lines": [...] } (last `lines` entries).
    std::function<nlohmann::json(int lines)> consoleLog;
    // Play state: { "mode": "edit|play", "paused": bool }.
    std::function<nlohmann::json()> playState;
    // Screenshot of the low-res FBO as a base64 PNG:
    // { "base64": "...", "mimeType": "image/png" } or { "error": ... }.
    std::function<nlohmann::json()> screenshot;
    // Drive play state: action ∈ "play"|"pause"|"resume"|"stop". Returns the
    // resulting play state ({ "mode", "paused" }) or { "error": ... }.
    std::function<nlohmann::json(const std::string& action)> control;
    // Reload the current scene from disk (discards live in-memory edits).
    // { "ok": true, "scenePath": ... } or { "error": ... }.
    std::function<nlohmann::json()> reloadScene;
    // Save the scene; empty path → current m_scenePath.
    // { "ok": true, "path": ... } or { "error": ... }.
    std::function<nlohmann::json(const std::string& path)> saveScene;

    // --- mutations (live, in-memory; persist with saveScene) -----------------
    // Emplace-or-replace component `comp` on the entity (entt id or Name) from
    // JSON. { "ok": true, ... } or { "error": ... }.
    std::function<nlohmann::json(const std::string& idOrName,
                                 const std::string& comp,
                                 const nlohmann::json& data)>
        setComponent;
    // Remove component `comp` from the entity. { "ok": true, ... } / error.
    std::function<nlohmann::json(const std::string& idOrName,
                                 const std::string& comp)>
        removeComponent;
    // Create a new entity (Name added when non-empty). { "ok": true, "id", ... }.
    std::function<nlohmann::json(const std::string& name)> createEntity;
    // Destroy the entity (entt id or Name). { "ok": true, "id" } / error.
    std::function<nlohmann::json(const std::string& idOrName)> destroyEntity;
    // Duplicate the entity (entt id or Name). { "ok": true, "id", "name" } / error.
    std::function<nlohmann::json(const std::string& idOrName)> duplicateEntity;

    // --- discovery / introspection -------------------------------------------
    // Every registered component with its field names, inferred types, and
    // default values: { "components": [ { "name", "fields":{...} } ] }.
    std::function<nlohmann::json()> listComponents;
    // Asset inventory: builtin meshes/textures, project texture files, shader
    // packs, and live runtime: keys. { "builtin_meshes":[...], ... }.
    std::function<nlohmann::json()> listAssets;
    // Available shader packs + each Camera's current pick:
    // { "packs":[...], "cameras":[ { "id","name","shader" } ] }.
    std::function<nlohmann::json()> listShaders;

    // --- human-in-loop feedback ----------------------------------------------
    // Select the entity in the editor (Inspector + viewport highlight).
    // { "ok": true, "id", "name" } / error.
    std::function<nlohmann::json(const std::string& idOrName)> selectEntity;
    // The current editor selection: { "id", "name" } or null.
    std::function<nlohmann::json()> getSelection;
    // Point the editor camera at the entity for a screenshot. { "ok": true } / err.
    std::function<nlohmann::json(const std::string& idOrName)> focusEntity;

    // --- scene I/O -----------------------------------------------------------
    // Open a different scene file (resolved via Assets; auto-stops Play).
    // { "ok": true, "scenePath" } / error.
    std::function<nlohmann::json(const std::string& path)> openScene;
    // Replace the current scene with a fresh blank one. { "ok": true }.
    std::function<nlohmann::json()> newScene;

    // --- verify / query + build ----------------------------------------------
    // Ray-vs-scene query over Collider/mesh AABBs. origin/dir are [x,y,z]
    // arrays, maxDist<=0 = unbounded. { "entity","point","normal","distance" }
    // or null.
    std::function<nlohmann::json(const nlohmann::json& origin,
                                 const nlohmann::json& dir, double maxDist)>
        raycast;
    // Check asset/script references resolve + exactly one primary Camera.
    // { "ok": bool, "issues":[...], "primaryCameraCount": n }.
    std::function<nlohmann::json()> validateScene;
    // Build the standalone game; empty path → a default under the project dir.
    // Synchronous + may exceed the response window — poll console_log for the
    // result. { "ok": true, "outPath" } / error.
    std::function<nlohmann::json(const std::string& outPath)> buildGame;
};

// PNG-encode RGBA8 pixels (bottom-up, w*h*4 bytes) and return the result
// base64-encoded. Defined in mcp_server.cpp; shared so the editor's screenshot
// provider getter can do the full encode on the main thread.
std::string mcpEncodePngBase64(const std::vector<unsigned char>& rgba, int w,
                               int h);

class McpServer {
public:
    // The provider's getters are invoked only from pump() (main thread); see the
    // threading note above. The server is not started until start() is called.
    explicit McpServer(McpProvider provider);
    ~McpServer(); // stops the server thread + joins.

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Bind 127.0.0.1 and spin the server thread. Tries `preferred` first, then a
    // few sequential ports if it's taken. Returns the bound port, or 0 if none
    // could be bound (logged via the sink). Idempotent: a second call is a no-op
    // returning the already-bound port.
    int start(int preferred = 7717);

    // Drain + run queued tool tasks on the calling (main) thread. Call once per
    // frame. Fulfills each task's promise so the blocked http thread wakes.
    void pump();

    int port() const { return m_port; }

    // Where logs go (defaults to nothing). Set before start() for visibility.
    void setLogSink(std::function<void(const std::string&)> sink) {
        m_log = std::move(sink);
    }

private:
    // A unit of work marshalled from an http thread onto the main thread.
    struct Task {
        std::function<void()> run;
    };

    // Run `fn` on the main thread and wait for it (default 5s). On timeout or
    // shutdown returns the fallback json. Called from http threads only.
    nlohmann::json marshal(std::function<nlohmann::json()> fn,
                           const nlohmann::json& onTimeout);

    // marshal() + wrap the JSON payload as an MCP text content block. On timeout
    // the (already MCP-shaped) timeout envelope is returned verbatim. `indent`
    // is forwarded to nlohmann::json::dump (-1 = compact). Shared by every
    // state/control/mutation tool so the marshal + timeout + textResult dance
    // lives in one place.
    nlohmann::json marshalText(std::function<nlohmann::json()> fn, int indent = -1);

    // JSON-RPC dispatch (runs on an http thread; only marshalled bits touch the
    // scene). Returns the JSON-RPC response object, or null for notifications.
    nlohmann::json handleRpc(const nlohmann::json& req);
    nlohmann::json callTool(const std::string& name, const nlohmann::json& args);

    void logLine(const std::string& s) const {
        if (m_log) m_log(s);
    }

    McpProvider m_provider;
    std::function<void(const std::string&)> m_log;

    std::unique_ptr<httplib::Server> m_svr;
    std::thread m_thread;
    std::atomic<int> m_port{0};
    std::atomic<bool> m_running{false};

    // Marshalling queue (http threads push, pump() drains).
    std::mutex m_queueMutex;
    std::queue<Task> m_queue;
};

} // namespace liminal::editor
