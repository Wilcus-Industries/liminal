---
name: liminal
description: Launch and drive the liminal game engine's headless editor to build a game. Use when asked to make/build/edit a game "in liminal" or "with the liminal editor" — this skill bootstraps the editor process, connects to its MCP server, and hands off to the per-project liminal-lua skill. Works from any directory, including an empty one.
---

# liminal: launching the headless editor (agent bootstrap)

liminal is a small C++ game engine with an ImGui editor, ECS scenes, Lua
scripting, and procedural generation. You build a game by driving the **headless
editor**: a GUI-less process that exposes an MCP (Model Context Protocol) server
over local HTTP. You introspect/mutate the live scene over MCP and author
`.lua` / `.lscene` files on disk with your normal Read/Write/Edit tools.

This skill only covers **getting connected**. Once connected, the editor seeds a
much fuller `liminal-lua` skill into the project (`.claude/skills/liminal-lua/`)
and writes `.mcp.json` — read that skill for the complete `lm` Lua API, the MCP
tool catalog, and the `.lscene` scene format.

## 1. Launch the editor (background)

Run the editor headless against a project directory. An **empty or new** directory
is auto-scaffolded into a project (`project.ljson` + `scenes/main.lscene`):

```sh
{{LIMINAL_EDITOR}} --headless --project <dir> --mcp-port {{MCP_PORT}} &
```

- `<dir>` = the game's project directory (use `.` for the current directory).
- Run it **in the background** (`&`) — it runs until killed (SIGINT/SIGTERM).
- `--mcp-port {{MCP_PORT}}` pins the MCP server to a known port. Omit it to let
  the editor pick (it prints the bound URL to stdout — see below). If the pinned
  port is busy the editor falls back and logs the real one.
- Requires a display server OR a display-less build. On a headless box (CI / bare
  SSH) the editor needs `LIMINAL_HEADLESS_OFFSCREEN=ON` at build time, else the GL
  context creation fails. Locally on a desktop it just works.

The editor echoes its logs to **stdout** in headless mode, including the line:

```
[mcp] server listening at http://127.0.0.1:{{MCP_PORT}}/mcp
```

Capture stdout (e.g. redirect to a file) and read that line to get the exact URL.

## 2. Connect to the MCP server

You have two ways to talk to the editor. **Pick curl for the first session** — it
needs no reconnect and works in every agent runtime.

### Option A — curl the JSON-RPC endpoint (no reconnect, recommended first)

The server is plain JSON-RPC 2.0 over a single `POST /mcp`, plaintext on
localhost. Drive every tool straight from the shell:

```sh
URL=http://127.0.0.1:{{MCP_PORT}}/mcp

# 1. initialize (once)
curl -s -X POST $URL -H 'content-type: application/json' -d '{
  "jsonrpc":"2.0","id":1,"method":"initialize",
  "params":{"protocolVersion":"2024-11-05","capabilities":{},
            "clientInfo":{"name":"agent","version":"1"}}}'

# 2. list the available tools
curl -s -X POST $URL -H 'content-type: application/json' -d '{
  "jsonrpc":"2.0","id":2,"method":"tools/list"}'

# 3. call a tool (e.g. read the scene tree)
curl -s -X POST $URL -H 'content-type: application/json' -d '{
  "jsonrpc":"2.0","id":3,"method":"tools/call",
  "params":{"name":"scene_tree","arguments":{}}}'
```

Every tool is request/response (no SSE). Pipe through your JSON tool of choice to
read results. This path needs **no MCP client setup and no reconnect** — it works
the moment the server is listening.

### Option B — native MCP integration (nicer, may need one reconnect)

The editor writes `<dir>/.mcp.json` registering the `liminal` server, and (for
Claude Code) installing this skill also pre-registered a user-scope `liminal`
server at the pinned port. If your session **started after** the editor was already
running, the server auto-connects — just call the `liminal` MCP tools directly.

If you launched the editor **mid-session**, your runtime won't see the new server
until it reloads MCP config. In Claude Code that's the `/mcp` command (a human
keystroke) or a restart. Until then, use Option A — it has no such limitation.

## 3. Hand off to the project skill

Once connected, the project directory now contains
`.claude/skills/liminal-lua/SKILL.md`. **Read it** — it documents:

- the full MCP tool catalog (~30 tools: `scene_tree`, `get_entity`,
  `list_components`, `list_assets`, `create_entity`, `set_component`,
  `play_game`, `screenshot`, `save_scene`, `build_game`, …),
- the `lm` Lua API for gameplay scripts (`lm.scene`, `lm.input`, `lm.time`,
  `lm.physics`, `lm.procgen`, `lm.render`, …),
- the `.lscene` scene-file JSON schema,
- the build/iterate loop and live-vs-persisted rules.

Follow its build loop: discover valid components/assets → read the scene →
mutate → `save_scene` → `validate_scene` / `screenshot` / `play_game` → iterate.
Author scripts as `.lua` files with your own file tools; the editor hot-reloads
them in Play.

## Quick start (from an empty directory)

```sh
# launch (auto-scaffolds an empty dir), capture logs
{{LIMINAL_EDITOR}} --headless --project . --mcp-port {{MCP_PORT}} > .liminal.log 2>&1 &
# wait a moment for the [mcp] listening line, then verify over curl
curl -s -X POST http://127.0.0.1:{{MCP_PORT}}/mcp -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"current_project","arguments":{}}}'
# read the seeded full skill and start building
cat .claude/skills/liminal-lua/SKILL.md
```
