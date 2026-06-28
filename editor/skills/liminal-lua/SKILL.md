---
name: liminal-lua
description: Build games in the liminal engine as an agent — the in-editor MCP tools for live scene introspection/mutation/play/screenshot, and the `lm` Lua API (scene, input, time, audio, procgen, AI, render, physics) for gameplay scripts. Use when editing a liminal project: authoring .lua scripts, hand-writing/repairing .lscene scenes, or driving the editor over MCP.
---

# liminal: scripting + the agent (MCP) workflow

There are two surfaces for building a liminal game, and you use both:

1. **The MCP tools** (this section) — talk to the **live editor**: read the
   current scene, mutate it in-memory, play it, and screenshot it. The editor
   exposes them over a local MCP server (auto-discovered via the project's
   `.mcp.json`).
2. **The `lm` Lua API** (the reference below) — what your gameplay **scripts**
   call at runtime. You author `.lua` files (and, when needed, `.lscene` scene
   files) with your own Read/Write/Edit tools.

**Division of labor:** MCP = *live editor state and the running scene*. Your own
file tools = *files on disk* (`.lua`, `.lscene`, shaders, textures). MCP has **no
file-authoring tools** by design — you already edit files directly. Conversely,
only MCP can see/mutate the live scene the human has open and drive Play.

## Contents

- [Driving liminal as an agent (MCP)](#driving-liminal-as-an-agent-mcp)
  - [The build loop](#the-build-loop)
  - [MCP tool catalog](#mcp-tool-catalog)
  - [Live vs persisted](#live-vs-persisted)
- [Conventions](#conventions)
- [Script file shape (lifecycle)](#script-file-shape-lifecycle)
- [The Entity handle](#the-entity-handle)
- [Components and their Lua fields](#components-and-their-lua-fields)
- [Scene file format (`.lscene`)](#scene-file-format-lscene)
- [Math types](#math-types)
- [`lm` API reference](#lm-api-reference)
- [Shaders](#shaders)
- [Examples](#examples)

## Driving liminal as an agent (MCP)

### Launching the editor (headless)

If the editor isn't already running, bring it up GUI-less and drive it over MCP.
An empty/new directory is auto-scaffolded into a project:

```sh
liminal-editor --headless --project <dir> [--mcp-port 7717] &
```

It runs until killed (SIGINT/SIGTERM) and echoes logs to **stdout**, including:

```
[mcp] server listening at http://127.0.0.1:<port>/mcp
```

Two ways to connect:

- **curl (no reconnect, works mid-session):** the server is JSON-RPC 2.0 over a
  single `POST /mcp`, plaintext on localhost. Drive any tool straight from the
  shell, e.g. `tools/call`:
  ```sh
  curl -s -X POST http://127.0.0.1:<port>/mcp -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
         "params":{"name":"scene_tree","arguments":{}}}'
  ```
  (First send `initialize`, then `tools/list` / `tools/call`.)
- **Native MCP:** the editor wrote `<dir>/.mcp.json` registering the `liminal`
  server. A session that **started after** the editor was running auto-connects.
  If you launched it mid-session, your runtime won't see it until it reloads MCP
  config (in Claude Code: `/mcp`, or restart) — use curl until then.

### The build loop

The reliable agent loop for building/iterating a scene:

1. **Discover** what's valid: `list_components` (exact component + field names
   and defaults — so `set_component` is never guesswork), `list_assets` (valid
   `meshAsset`/`textureAsset` names), `list_shaders` (valid `Camera.shaderName`),
   `list_scenes` (paths for `open_scene`).
2. **Read** current state: `scene_tree`, then `get_entity` for detail.
3. **Mutate** (live, in-memory): `create_entity` → `set_component` (guided by
   `list_components`) → `duplicate_entity` / `remove_component` / `destroy_entity`.
4. **Show the human** what you touched: `select_entity` (highlights in the
   Inspector + viewport), `focus_entity` (points the editor camera at it).
5. **Persist**: `save_scene` (mutations are NOT auto-saved).
6. **Verify**: `validate_scene` (catches unresolved assets / missing scripts /
   bad primary-camera count), `screenshot`, `play_game` + `console_log`, then
   `stop_game`. Iterate.

When you write a `.lua` script with your own tools, the editor hot-reloads it in
Play (~0.5s); use `console_log` to read its `lm.log` output and any `[lua]`
errors. After editing a `.lscene` on disk, call `reload_scene` to pull it into
the live editor.

### MCP tool catalog

All tools operate on the **current** scene/project. *kind* — **read** (no
change), **control** (play state / scene I/O), **mutate** (live in-memory edit,
needs `save_scene` to persist).

| Tool | Input | Returns | Kind |
|---|---|---|---|
| `scene_tree` | — | `{entities:[{id,name,components[]}]}` | read |
| `get_entity` | `{id}` (entt id or Name) | `{id,components:{Name:{...}}}` | read |
| `current_project` | — | `{projectFile,title,scenePath}` | read |
| `list_components` | — | `{components:[{name,fields:{f:{type,default}}}]}` | read |
| `list_assets` | — | `{builtin_meshes,builtin_textures,project_textures,runtime_meshes,runtime_textures,shader_packs}` | read |
| `list_scenes` | — | `["scenes/x.lscene", ...]` | read |
| `list_shaders` | — | `{packs:[...],cameras:[{id,name,shader,primary}]}` | read |
| `list_scripts` | — | `["scripts/x.lua", ...]` | read |
| `read_script` | `{path}` | file text (≤512 KiB, sandboxed to project) | read |
| `console_log` | `{lines?}` (default 200) | `{lines:[...]}` | read |
| `play_state` | — | `{mode:"edit"\|"play",paused}` | read |
| `get_selection` | — | `{id,name}` or `null` | read |
| `raycast` | `{origin:[x,y,z],dir:[x,y,z],maxDist?}` | `{entity,name,point,normal,distance}` or `null` | read |
| `validate_scene` | — | `{ok,issues:[{entity,name,severity,message}],primaryCameraCount}` | read |
| `screenshot` | — | MCP image block (PNG of the scene FBO, 1 frame stale) | read |
| `select_entity` | `{id}` | `{ok,id,name}` | control |
| `focus_entity` | `{id}` | `{ok,id}` (aims the editor camera; edit mode) | control |
| `play_game` | — | `{mode,paused}` | control |
| `pause_game` | `{paused?}` (default true) | `{mode,paused}` | control |
| `stop_game` | — | `{mode,paused}` (restores pre-Play scene) | control |
| `send_input` | `{keys_down?,keys_up?,mouse_down?,mouse_up?,look_dx?,look_dy?,hold_ms?,capture?}` | `{ok,applied}` (Play only) | control |
| `open_scene` | `{path}` | `{ok,scenePath}` (auto-stops Play) | control |
| `new_scene` | — | `{ok}` (blank, unsaved) | control |
| `reload_scene` | — | `{ok,scenePath}` (re-reads disk, discards live edits) | control |
| `save_scene` | `{path?}` (default current) | `{ok,path}` | control |
| `create_entity` | `{name?}` | `{ok,id,name}` | mutate |
| `set_component` | `{id,component,data?}` | `{ok,id,component}` | mutate |
| `remove_component` | `{id,component}` | `{ok,id,component}` | mutate |
| `duplicate_entity` | `{id}` | `{ok,id,name}` (copies all components) | mutate |
| `destroy_entity` | `{id}` | `{ok,id}` | mutate |
| `build_game` | `{path?}` | `{ok,outPath}` (synchronous; poll `console_log`) | control |

Notes:
- `set_component` data uses the **serialized JSON field names** (e.g.
  `meshAsset`, `rotationEuler`), NOT the Lua field names — `list_components`
  reports the exact names + defaults. On `"Script"`, `data` is `{"paths":[...]}`.
- All mutations are **undoable** in the editor (Cmd/Ctrl+Z) — they push an undo
  snapshot, same as a manual edit.
- `build_game` runs on the main thread and a large build may exceed the response
  window (you get a timeout); the build still completes — poll `console_log`.
- `raycast` resolves over each entity's `Collider` (or mesh bounds); the same
  query `lm.physics.raycast` does at runtime.

### Playing the game yourself (`send_input`)

You can **play the running game**, not just start it. The loop:

1. `play_game` — enter Play (scripts run).
2. `send_input` — inject keyboard/mouse the scripts read via `lm.input.*`.
3. `screenshot` — observe the result; decide the next move.
4. Repeat 2–3, then `stop_game`.

`send_input` fields (all optional, batched in one call):
- `keys_down` / `keys_up` — key names: single chars (`"w"`,`"a"`) or
  `space`,`shift`,`ctrl`,`alt`,`enter`,`tab`,`esc`,`up`,`down`,`left`,`right`.
- `mouse_down` / `mouse_up` — button indices (`0`=left,`1`=right,`2`=middle).
- `look_dx` / `look_dy` — a one-shot mouse-look delta (for `lm.input.mouse_delta`).
- `hold_ms` — auto-release the pressed keys/buttons after N ms. **`0` (default)
  = held until you release** with a matching `keys_up`/`mouse_up`. Use a hold for
  sustained movement ("walk while I look around"), `hold_ms` for a discrete tap
  (jump, shoot).
- `capture: true` — make `lm.input.cursor_captured()` report true so FPS scripts
  that gate mouse-look on capture respond. Set it once after `play_game`.

Injected input **ORs with** any real input — in a normal editor window it adds to
human keys; in `--headless` it is the only source, so an agent plays fully
autonomously over HTTP. Held keys are auto-released on `stop_game`.

Example — walk forward a beat, then stop:
```
play_game
send_input {"keys_down":["w"],"capture":true}
screenshot                    # see the world move
send_input {"keys_up":["w"]}
stop_game
```

### Live vs persisted

Mutations change the **in-memory** scene the human sees, but the `.lscene` file
on disk is unchanged until you `save_scene`. `reload_scene` throws away live
edits and re-reads disk. `play_game` snapshots first and `stop_game` restores, so
Play never corrupts your edit-mode scene.

## Conventions

- **Asset keys.** `MeshRenderer.meshAsset` / `textureAsset` accept: a
  `builtin:*` name (see `list_assets`), a `runtime:*` key (from
  `lm.assets.add_*`), or — for textures only — a project-relative file path
  (PNG/JPEG/TGA/BMP, read through the VFS). There is no mesh **file** loader: a
  non-`builtin:`/non-`runtime:` mesh name does not resolve.
- **Exactly one primary camera.** A scene should have a single `Camera` with
  `primary: true`; `validate_scene` warns otherwise. Its `shaderName` selects the
  render pack (`native` default, `retro`, or a discovered custom pack).
- **Determinism in procgen.** Every `lm.procgen` entry point takes an explicit
  seed or `Rng`; never use `math.random` for world generation (a determinism
  test gates this engine-wide).
- **JSON field names ≠ Lua field names.** Serialized (`.lscene` / `set_component`)
  uses `meshAsset`, `textureAsset`, `rotationEuler`, `fovDeg`/`nearZ`/`farZ`; the
  Lua usertypes use `.mesh`, `.texture`, `.rotation`, `.fov`/`.near`/`.far`. When
  in doubt, call `list_components`.
- **Rotations are degrees**, Euler order yaw→pitch→roll = **Y→X→Z**, stored as
  `[pitch, yaw, roll]` (= `x, y, z`) in the array.
- **Frame-local component pointers.** `get_*`/`get_component` return raw entt
  pointers valid only for the current frame — re-fetch each `on_update`, never
  cache across frames or structural changes.
- **Textures don't hot-reload** (only shaders do). Editing a texture file on disk
  won't refresh the screen — re-call `lm.assets.add_texture(name, path)` with the
  same name, or reopen the project.
- **`lm.ai` requires `forget(id)`** after each completed request or responses
  leak for the engine's lifetime.

## Script file shape (lifecycle)

liminal runs Lua 5.4 (via sol2). Each entity with a `Script` component points at
one or more `.lua` files. A file is loaded in a fresh per-entity environment and
**must return a table** with optional `on_start` / `on_update` callbacks. The
`lm` global is the entire runtime API.

The host calls:

- `on_start(self)` — once, the first frame the entity's script is live (and again
  after a hot reload). `self` is the owning `Entity`.
- `on_update(self, dt)` — every frame. `dt` is the frame delta in **seconds** (a
  number passed by the engine — there is no `lm.time.dt`).

Anything else in the table is your own state/helpers. File-scope locals are
per-entity (fresh environment per instance), so module-level config is fine.

```lua
local M = {}

function M.on_start(self)
    lm.log("hello from " .. self.name)
end

function M.on_update(self, dt)
    -- gameplay here
end

return M
```

Returning anything other than a table is an error. `on_start` / `on_update` are
both optional, but a script with neither does nothing.

**Hot reload:** in the editor (Play mode) the file is watched (~0.5s); saving
re-inits all instances of that script (`on_start` runs again). The shipped player
has hot reload off.

**Error handling:** every callback is pcall'd. Errors are logged once
(deduplicated, visible via `console_log`) and the script keeps running; fixing
the file and saving re-reports if it still errors.

## The Entity handle

`Entity` is a lightweight value handle (copyable). Methods:

- `self.name` — read/write the `Name` component (string property). Writing adds a
  `Name` if missing.
- `self:valid()` — is the entity still alive.
- `self:destroy()` — destroy this entity.
- `self:get_transform()` — returns the `Transform` (auto-adds one if missing, so
  never nil on a valid entity).
- `self:get_mesh_renderer()` — returns the `MeshRenderer` or **nil** if absent
  (renderers are never auto-added; always nil-check).
- `self:get_component("Name")` — generic accessor for any registered component by
  name; returns the component or nil.
- `self:add_component("MeshRenderer", { mesh = "builtin:box" })` — add (or
  replace) a component by name from a table of fields; returns the new component
  handle (frame-local). The optional table mirrors the component's fields. This
  is how a script gives a freshly-created entity a `MeshRenderer` (or `Collider`,
  `Light`, …) — no scene pre-authoring needed.
- `self:remove_component("Light")` — remove a component by name (no-op if absent).
- `self:has_component("Collider")` → bool.

**Frame-local pointer caveat (important):** components returned by `get_transform`
/ `get_mesh_renderer` / `get_component` are raw pointers into the entt registry,
valid only for the current frame and dangling after any structural change
(add/remove component, create/destroy entity). **Never cache a component across
frames** — re-fetch it each `on_update`.

## Components and their Lua fields

Fetch via the accessors above. Field names (Lua usertype side):

- **Name**: `.value` (string).
- **Transform**: `.position`, `.rotation`, `.scale` (all `vec3`; rotation in
  **degrees**). Also `t:set_position(x,y,z)`, `t:set_rotation(x,y,z)`,
  `t:set_scale(x,y,z)`. Mutating `t.position.y = 2` mutates the real component
  (vecs return by reference).
  - **Euler order gotcha:** rotation applied yaw→pitch→roll = **Y → X → Z**
    (`rotation.y` is yaw, `.x` pitch, `.z` roll). Spin "around vertical" = change
    `rotation.y`.
- **MeshRenderer**: `.mesh` (asset string), `.texture` (asset string), `.color`
  (`vec4`, gradient **bottom** tint), `.color2` (`vec4`, gradient **top** tint —
  blended up the mesh's local height). Also `mr:set_color(r,g,b[,a])` (which
  **also** sets `color2`, so a flat tint unless you set a gradient) and
  `mr:set_color2(r,g,b[,a])` (top color only — call after `set_color` for a
  base→top gradient). Mesh names:
  `builtin:box|pyramid|pillar|arch|stair|plane|quad`, seeded primitives like
  `builtin:blob:42` / `builtin:tree:7` / `builtin:rock:3` / `builtin:crystal:5`,
  the parametric `builtin:form:<sides>,<twist>,<taper>[,<seed>]` (n-gon prism,
  sides 3..8, e.g. `builtin:form:6,0.2,0.5`), or a `runtime:` key.
- **Camera**: `.fov`, `.near`, `.far` (numbers), `.primary` (bool), `.shader`
  (string — the shader pack name). Fetch via `self:get_component("Camera")`.
- **Light**: `.color` (`vec3`), `.intensity` (number). Also
  `light:set_color(r,g,b)`.
- **Collider**: `.center` (`vec3`), `.half_extents` (`vec3`) — an axis-aligned
  box for `lm.physics`. Zero `half_extents` = "derive from the mesh bounds" at
  query time.
- **Billboard**: `.yaw_only` (bool). Presence makes the entity face the active
  camera each frame — yaw-only when true, full-facing (also pitch) when false.

`AudioSource` and `Script` are registered components but expose no Lua usertype
fields beyond `get_component` — drive audio/AI through the `lm.*` namespaces.

## Scene file format (`.lscene`)

A scene is a JSON document (`scenes/*.lscene`). **Prefer the MCP tools**
(`create_entity`, `set_component`, … then `save_scene`) — they always emit a
valid document and `validate_scene` checks it. Hand-writing a `.lscene` is a
fallback; if you do, you **must** include the top-level `"liminal_scene": 1` key
or the load fails with `not a liminal_scene v1 document`.

**Required top-level schema:**

```json
{
  "liminal_scene": 1,
  "entities": [
    {
      "id": 0,
      "components": {
        "Name": { "value": "..." },
        "Transform": { "position": [0,0,0], "rotationEuler": [0,0,0], "scale": [1,1,1] }
      }
    }
  ]
}
```

Notes:
- `"liminal_scene": 1` is the version marker — **load fails without it.**
- `id` is debug-only and **not** preserved across a load: entities get fresh entt
  ids in file order. Never rely on a specific id.
- `components` maps a registry component **name** → that component's JSON body.
- An unknown component name **warns on stderr and is skipped** (never fatal).
- JSON field names are the **serialized** names (see `list_components`), differing
  from the Lua usertype names: `meshAsset`/`textureAsset` (not `.mesh`/`.texture`),
  `rotationEuler` (not `.rotation`), `fovDeg`/`nearZ`/`farZ` (not `.fov`/etc).
- `Transform.rotationEuler` is **degrees**, applied Y→X→Z (so the array is
  `[pitch, yaw, roll]` = `x, y, z`).

**Component JSON bodies** (defaults shown; omit a field to take its default):
- `Name`: `{ "value": "crate" }`
- `Transform`: `{ "position": [0,0,0], "rotationEuler": [0,0,0], "scale": [1,1,1] }`
- `Camera`: `{ "fovDeg": 70.0, "nearZ": 0.1, "farZ": 220.0, "primary": true, "shaderName": "native" }`
- `MeshRenderer`: `{ "meshAsset": "builtin:box", "color": [1,1,1,1], "color2": [1,1,1,1], "textureAsset": "builtin:white" }`
  (`color` = gradient bottom, `color2` = top; omit `color2` to match `color`.
  `textureAsset` may be `""` for untextured.)
- `Light`: `{ "color": [1,1,1], "intensity": 1.0 }`
- `Script`: `{ "paths": ["scripts/foo.lua"] }` (one entity may list several)
- `Collider`: `{ "center": [0,0,0], "halfExtents": [0,0,0] }` (zero `halfExtents`
  = derive from mesh bounds at query time)
- `Billboard`: `{ "yawOnly": true }`
- `AudioSource`: `{ "gain": 0.14, "enabled": true }`

**Minimal valid scene** — a primary camera + one textured cube + a light:

```json
{
  "liminal_scene": 1,
  "entities": [
    {
      "id": 0,
      "components": {
        "Name": { "value": "player" },
        "Camera": { "fovDeg": 75.0, "nearZ": 0.1, "farZ": 220.0, "primary": true, "shaderName": "native" },
        "Script": { "paths": ["scripts/player.lua"] },
        "Transform": { "position": [0,2,6], "rotationEuler": [-12,0,0], "scale": [1,1,1] }
      }
    },
    {
      "id": 1,
      "components": {
        "Name": { "value": "cube" },
        "MeshRenderer": { "meshAsset": "builtin:box", "color": [0.8,0.8,0.85,1.0], "textureAsset": "builtin:concrete" },
        "Transform": { "position": [0,0.5,0], "rotationEuler": [0,0,0], "scale": [1,1,1] }
      }
    },
    {
      "id": 2,
      "components": {
        "Name": { "value": "light" },
        "Light": { "color": [1,1,1], "intensity": 1.0 },
        "Transform": { "position": [3,5,2], "rotationEuler": [0,0,0], "scale": [1,1,1] }
      }
    }
  ]
}
```

Swap scenes at runtime with `lm.scene.change("scenes/x.lscene")` — it loads the
target in the standalone player **and in editor Play** (the swap is deferred to
just after the per-frame script update). In editor Play, pressing Stop restores
the original pre-Play scene (the swapped-to scene is discarded on Stop).

## Math types

- `vec3(x,y,z)` (or `vec3()`, `vec3(s)`) — fields `.x .y .z`, supports `+ - *`
  (scalar), unary `-`, `:length()`, `tostring`.
- `vec4(x,y,z,w)` — fields `.x .y .z .w`.
- Available as `lm.vec3` / `lm.vec4` as well as bare `vec3` / `vec4`.
- Lua stdlib `math`, `string`, `table`, the **full `os`** and **full `io`** are
  open — scripts have complete standard-library file access. **Caveat:** `io`
  reads the real OS filesystem, **not** the mounted pak, so in a shipped game an
  `io.open` cannot read a file packed into the `.pak`. For structured data prefer
  `lm.json`; to load another script's table use `lm.import`.

## `lm` API reference

### lm.log(msg)
Print to the engine console (visible via the `console_log` MCP tool).

### lm.json
- `lm.json.encode(value)` → JSON string. Tables encode as a JSON array when their
  keys are a contiguous `1..n` run, else an object. Functions/userdata → `null`.
- `lm.json.decode(string)` → Lua value. **Raises** a Lua error on malformed JSON —
  wrap in `pcall` if the source is untrusted (e.g. LLM output). Pairs with
  `lm.ai` JSON specs.

### lm.import(path)
Project-relative module loader (the VFS-aware analogue of `require`): reads `path`
through the asset system (pak first, then search paths), runs the chunk **once**
in the shared Lua state, and caches its return value. Every
`lm.import("scripts/util.lua")` from any entity script returns the **same** cached
table — the supported way to share code AND mutable state across scripts (entity
scripts otherwise each get an isolated environment). The module should `return` a
table. Re-entrant imports of a still-loading file get `nil` (cycle guard); a
read/compile/runtime error is logged and returns `nil` (never throws).

```lua
-- scripts/shared.lua
return { score = 0, add = function(self, n) self.score = self.score + n end }

-- any entity script
local shared = lm.import("scripts/shared.lua")
shared:add(10)   -- visible to every other script that imported it
```

### lm.scene
- `lm.scene.find(name)` → first `Entity` with that `Name`, or nil.
- `lm.scene.find_all(name)` → array (1-based) of all matching entities.
- `lm.scene.create(name)` → new `Entity` (gets a `Name`).
- `lm.scene.each(fn)` → calls `fn(entity)` for every live entity. Iteration is
  snapshotted, so creating/destroying entities inside `fn` is safe.
- `lm.scene.destroy(entity)` → destroy an entity.
- `lm.scene.change(path)` → request a scene swap. Honored in the standalone
  player **and** in editor Play (deferred to just after the script update). In
  editor Play, Stop restores the pre-Play scene.

### Coordinate system & camera math

**Read this before writing any movement or camera code.** Most first-person bugs
(W/S reversed, strafe mirrored, mouse-look inverted) come from guessing the
forward/right vectors. Don't guess — use the formulas below.

The world is **right-handed**: **+X** right, **+Y** up, **+Z** toward the viewer.
A camera (or any entity) with zero rotation looks down **−Z**. Rotations are
degrees, Euler order yaw→pitch→roll = **Y→X→Z** (`rotation.y` = yaw, `.x` = pitch,
`.z` = roll — see the Transform note above). The engine builds a Transform as
`T·Ry·Rx·Rz·S` and a Camera's view as `inverse(transform)`, so the world basis a
script must use, derived from `rotation`, is:

```lua
local t = self:get_transform()              -- re-fetch every frame, never cache
local yaw, pitch = math.rad(t.rotation.y), math.rad(t.rotation.x)
-- forward the entity faces (full 3D, includes pitch)
local fwd   = vec3(-math.sin(yaw) * math.cos(pitch),
                    math.sin(pitch),
                   -math.cos(yaw) * math.cos(pitch))
-- horizontal forward (ignore pitch) for walking on the ground plane
local fwdH  = vec3(-math.sin(yaw), 0, -math.cos(yaw))
-- right (strafe), always horizontal
local right = vec3( math.cos(yaw), 0, -math.sin(yaw))
```

Sanity check: at `yaw=0` `fwdH=(0,0,-1)`, `right=(1,0,0)`; at `yaw=90°`
`fwdH=(-1,0,0)`, `right=(0,0,-1)`.

**Movement (WASD):** `W = +fwdH`, `S = -fwdH`, `D = +right`, `A = -right`. Sum the
keys into a move vector, normalize it (so diagonals aren't faster), then
`t.position += move * speed * dt`.

**Mouse-look sign rules** (these exact signs are the usual failure points):
- `yaw   = yaw   - dx * sens` — drag right (`dx>0`) turns right.
- `pitch = pitch - dy * sens` — mouse up looks up.
- Clamp `pitch` to about **±89°** so you never flip over the pole.
- `dx, dy` come from `lm.input.mouse_delta()` and are only non-zero while the
  cursor is captured (`lm.input.set_cursor_captured(true)`).

**Trap — do NOT copy the engine's C++ editor camera.** The editor's free-fly
camera (`editor_app.cpp`) uses a *different* internal convention
(`fwd = (+sin yaw, …, +cos yaw)` via `glm::lookAt`) that does **not** match a
Transform-driven entity. Use the Lua formulas above, not the C++ editor source.

See the **First-person controller** example at the end for a full script.

### lm.input
- `lm.input.key_down(key)` → bool. `key` is a GLFW keycode (number) or a single
  character string (`"w"`, `"A"`). Always false in windowless/headless hosts.
- `lm.input.mouse_down(button)` → bool. `button` is a GLFW mouse button (0 = left,
  1 = right, 2 = middle).
- `lm.input.mouse_delta()` → `dx, dy`, accumulated cursor movement since the last
  call (zeroed on read). **Only non-zero while the cursor is captured.**
- `lm.input.set_cursor_captured(captured)` — hide + lock the cursor for FPS-style
  mouse-look (true) or release it (false). Works in the player and editor Play.
  Typical: capture in `on_start`, toggle with a key so the user can reach UI. On
  Stop the editor releases the cursor for you.
- `lm.input.cursor_captured()` → bool.

### lm.time
- `lm.time.now()` → seconds since the host started (monotonic, double).
- There is **no** `lm.time.dt` — use the `dt` argument to `on_update`.

### lm.ui  (immediate-mode 2D / HUD)
Screen-space drawing, re-issued every frame from `on_update` (immediate mode —
nothing persists). Drawn into the scene framebuffer, so it shows in the player,
the editor viewport, and MCP screenshots. Coordinates are **render-target pixels,
origin top-left** (x→right, y→down). Colors are 0..1 floats; alpha defaults to 1.
- `lm.ui.size()` → `w, h` — current render-target pixel size (for `retro` this is
  the 400×300 virtual size, not the window).
- `lm.ui.rect(x, y, w, h, r, g, b [, a])` — filled rectangle. `lm.ui.quad` aliases.
- `lm.ui.text(x, y, str, r, g, b [, a [, scale]])` — bitmap text (engine-baked 8×8
  font; `scale` multiplies the 8px cell). `x,y` is the top-left of the first glyph.
- `lm.ui.line(x0, y0, x1, y1, r, g, b [, a [, thickness]])` — line segment.

```lua
function M.on_update(self, dt)
    local w, h = lm.ui.size()
    lm.ui.rect(0, 0, w, 18, 0, 0, 0, 0.5)          -- HUD bar
    lm.ui.text(6, 5, "SCORE " .. score, 1, 1, 1)   -- white text
    lm.ui.line(w/2 - 4, h/2, w/2 + 4, h/2, 1, 1, 1) -- crosshair
end
```

### lm.render  (per-frame render uniforms)
Drives the shared scene-shader knobs from Lua so a custom pack can react per area
/ to a decay clock. Persist until changed (set each frame, or once on change).
- `lm.render.set_fog(r, g, b, density)` — fog color + density (`uFogColor` /
  `uFogDensity`).
- `lm.render.set_decay(p)` — a 0..1 "unravel" progress (`uDecayProgress`).
- `lm.render.set_light(x, y, z)` — primary light direction (`uLightDir`).
- `lm.render.set_shade(x, y, z)` — secondary/disagreeing light (`uShadeDir`).
- `lm.render.set_uniform(name, number)` / `set_uniform(name, vec3)` — set an
  arbitrary named uniform on the active scene shader. Unknown names (not declared
  by the current pack) are silently ignored.

### lm.physics  (raycast / overlap)
World queries against entity boxes — each entity's `Collider` (if present and
non-degenerate) else its mesh's local bounds.
- `lm.physics.raycast(origin, dir [, maxDist])` → `{ entity, point, normal,
  distance }` or `nil`. `origin`/`dir` are `vec3` (`dir` normalized for you);
  `maxDist` 0/omitted = unbounded. `entity` is the hit `Entity`; `point`/`normal`
  are world-space `vec3`; `distance` is along `dir`.
- `lm.physics.overlap(a, b)` → bool — do the world-space AABBs of entities `a`/`b`
  intersect (false if either has no resolvable box).

```lua
-- eye ray pick (horizontal forward; see "Coordinate system & camera math")
local t = self:get_transform()
local fwd = vec3(-math.sin(t.rotation.y), 0, -math.cos(t.rotation.y))
local hit = lm.physics.raycast(t.position, fwd, 20.0)
if hit then lm.log("looking at " .. hit.entity.name) end
```

### lm.audio
One procedural DSP voice bank (not positional sources). The game thread only pokes
atomics; these calls are safe.
- `lm.audio.set(name, value)` — set a parameter. Float names: `gain`, `decay`,
  `dread`, `breath`, `lamp_hum`, `zone_blend`, `ambient_gain`, `water_gain`,
  `throb_level`, `breath_level`, `step_level`, `water_level`, `dropout_min`,
  `dropout_max`, `interval_min`, `interval_max`. Int names: `zone_a`, `zone_b`,
  `water_tag`. Bool: `enabled`. Unknown names warn once and no-op.
- `lm.audio.get(name)` → current value (0 if unknown / no audio).
- `lm.audio.event(name)` — fire a one-shot. Names: `step`, `jump`, `mumble`,
  `door_creak`.
- `lm.audio.ok()` → bool, whether the audio device is running.

### lm.assets
- `lm.assets.add_mesh(name, meshData)` → returns the resolvable storage key (a
  `runtime:` string) to put in `MeshRenderer.mesh`. `meshData` is a `MeshData`
  from procgen (e.g. `piece.mesh`, `lm.procgen.terrain_mesh`).
- `lm.assets.add_texture(name, path)` → loads an image (PNG/JPEG/TGA/BMP) through
  the VFS and returns its `runtime:` key (put in `MeshRenderer.texture`).
- `lm.assets.add_texture(name, w, h, pixels)` → builds a `w×h` RGBA8 texture from
  a flat Lua array `pixels` (length `w*h*4`, 0..255, row-major, top-left origin);
  returns the `runtime:` key.
- **Persistence:** runtime meshes/textures live for the asset cache's lifetime —
  they **survive** scene reloads, play/stop, and `lm.scene.change`. Lost only on
  project close / process exit. Re-registering in `on_start` is safe (idempotent)
  but not required for a key to resolve after a reload.
- **No texture hot-reload:** a texture file is decoded **once** into a GPU
  resource and cached. Editing the file on disk does not refresh the screen (no
  scene reload / play-stop / `lm.scene.change` / `reload_scene` rebuilds the
  cache). Re-call `add_texture(name, path)` with the same `name`, use a fresh
  name, or reopen the project.

### lm.ai  (only when built with `LIMINAL_WITH_INFERENCE`)
Local LLM inference. The table may be absent — feature-test with `if lm.ai then`.
The engine pointer may also be null (tests / opted-out host): calls warn once and
return sensible defaults.
- `lm.ai.start{ model = "path", context = N, threads = N, gpu_layers = N }` — load
  a model (`model` required, resolved through Assets).
- `lm.ai.stop()`.
- `lm.ai.status()` → `status, message` (status `"stopped"|"loading"|"ready"|"failed"`).
- `lm.ai.submit{ system=, user=, grammar=, temperature=, top_p=, top_k=,
  repeat_penalty=, max_tokens=, seed= }` → request id (integer).
- `lm.ai.poll(id)` → `{ text=, complete=, failed=, error= }`.
- `lm.ai.cancel(id)`, `lm.ai.busy()` → bool, `lm.ai.queue_depth()` → int.
- `lm.ai.forget(id)` — **required** after a completed request. Finished responses
  are retained until forgotten; never forgetting leaks responses for the engine's
  lifetime.

### lm.procgen
Deterministic procedural generation. **Hard rule:** every entry point takes an
explicit seed (or an `Rng`); never use Lua `math.random` for world generation.

Primitives:
- `lm.procgen.rng(seed)` → `Rng` (xorshift32). `rng:next()` (uint32),
  `rng:next01()` (0..1), `rng:range(lo, hi)`.
- `lm.procgen.tileset(jsonPath?)` → `TileSet` (default set if no path). Methods:
  `:count()`, `:name(id)`, `:id_of(name)`, `:role_id(role)`, `:walkable(id)`.

Terrain:
- `lm.procgen.terrain{ seed=, kind=, water=, n=, tile_size= }` → `HeightField`.
  `kind`: `plane|hills|canyon|islands|flooded|void`. `water`: `none|some|lots`.
  `HeightField`: `.nodes`, `.cell`, `.half`, `.has_water`, `.water_level`,
  `:tile_height(x,z)`, `:tile_underwater(x,z)` (tile/node indices), and the
  **world-space** `:height_at_world(worldX, worldZ)` (bilinear) and
  `:walkable_at_world(worldX, worldZ)` (not underwater). Use the world-space pair
  to place objects/the player on the ground without deriving the tile transform.
- `lm.procgen.terrain_mask(hf, params, tileset)` → opaque tile masks.
- `lm.procgen.terrain_mesh(hf)` / `lm.procgen.water_mesh(hf)` → `MeshData` (hand to
  `lm.assets.add_mesh`).

WFC layout:
- `lm.procgen.stamp_footprint(masks, tileset, n, cx, cz, w, d)` → `FootprintPlan`
  or nil.
- `lm.procgen.solve_wfc{ tileset=, masks=, n=, seed=, max_restarts= }` →
  `WfcResult` (`.solved`, `.restarts`).
- `lm.procgen.grid_from(wfcResult, n, tileSize)` → `TileGrid` (`.n`, `.tile_size`,
  `:in_bounds(x,z)`, `:at(x,z)`, `:set(x,z,id)`, `:center(x,z)` → `worldX, worldZ`).
- `lm.procgen.validate(grid, hf, sites, plans, tileset)` →
  `{ repairs=, unreachable_zones=, plans= }`. `sites` an array of `{x=,z=}`;
  `plans` an array of `FootprintPlan`. Mutates `grid`; returns door-filled plans.
  Never fails (always repairs).

Architecture:
- `lm.procgen.build_building(grid, hf, footprintPlan, familyParams, rng)` →
  `BuiltPiece`.
- `lm.procgen.collect_runs(grid, tileset)` → array of `DeckRun` (`.chain_len`,
  `.is_pier`).
- `lm.procgen.build_deck(grid, hf, tileset, deckRun)` → `BuiltPiece`.
- `FootprintPlan{ zone=, style=, x0=, z0=, w=, d=, door_x=, door_z= }`,
  `FamilyParams{ floor_height=, wall_thickness=, foundation=, min_floors=,
  max_floors=, roof=, ruin_chaos=, colonnade=, stairs=, lamp_chance= }`
  (constructed from a table).
- `BuiltPiece`: `.mesh` (`MeshData`), `.zone`, `.anchor` (`vec3`),
  `:vertex_count()`, `:door_count()`, `:lamp_count()`, `:lamps()` (array of vec3).

One-shot:
- `lm.procgen.town{ seed=, n=, tile_size=, terrain=, water=, tileset=, families= }`
  → `{ grid=, terrain=, pieces=, repairs=, restarts= }`. Runs the full
  terrain→mask→WFC→validate→architecture pipeline deterministically. `pieces` is
  an array of `BuiltPiece`.

## Shaders

The renderer has a named shader-pack registry. Two built-ins are always
available: **`native`** (default — clean, perspective-correct, crisp) and
**`retro`** (the PS1 look — low-res FBO, vertex snap, affine UVs, fog). A `Camera`
picks its pack by name via `shaderName` (a **dropdown in the Camera inspector**;
`list_shaders` reports valid names).

### Authoring custom shaders

Drop GLSL under `<projectDir>/shaders/`. Two layouts, both auto-discovered on
project open (and **hot-reloaded ~0.5s after you save** — a compile error is
logged and the previous working shader is kept). Both appear in the Camera
dropdown by name. Custom shaders render at native (window) resolution.

**Frag-only — `shaders/<name>.frag`** (dropdown name = file stem). A fragment
**BODY ONLY**: no `#version`, no `in`/`out`/`uniform` declarations. The engine
prepends them and supplies the vertex stage. You write just `void main(){ ... }`.
Available to your body:

```glsl
in vec3 vNormal;       // world-space normal
in float vViewDist;    // radial distance from the camera
in float vGradT;       // 0 at the object's base, 1 at its top
smooth in vec2 vUV;    // perspective-correct texcoords
uniform sampler2D uTex;
uniform vec3 uColor;   // gradient bottom tint
uniform vec3 uColor2;  // gradient top tint
uniform vec3 uLightDir;
uniform float uAlphaTest; // >0.5 = hard cutout where texel alpha < 0.5
out vec4 FragColor;
```

Minimal example (`shaders/grayscale.frag`):

```glsl
void main() {
    vec3 c = texture(uTex, vUV).rgb * mix(uColor, uColor2, vGradT);
    float g = dot(c, vec3(0.299, 0.587, 0.114));
    float lambert = max(dot(normalize(vNormal), normalize(uLightDir)), 0.0);
    FragColor = vec4(vec3(g) * (0.35 + 0.65 * lambert), 1.0);
}
```

**Full pack — `shaders/<name>/scene.vert` + `scene.frag`** (name = dir name). You
own both stages. Vertex attributes are location `0 = pos (vec3)`,
`1 = normal (vec3)`, `2 = uv (vec2)`. You may read any per-draw uniform the
renderer sets: `uModel, uView, uProj, uNormalMat, uColor, uColor2, uGradBase,
uGradInv, uTex, uLightDir` (plus retro-only ones — unused are harmless). The
blit/upscale stage is engine-provided.

### Switching a camera's shader at runtime

The `Camera` component is Lua-bound. Get it and set `.shader`:

```lua
local cam = self:get_component("Camera")
if cam then cam.shader = "grayscale" end  -- or "native" / "retro" / a full pack
```

(`get_component` returns a frame-local pointer — set it the frame you fetch it,
don't cache it.)

## Examples

### Spin component (transform + tint pulse)

```lua
local speed = 90.0 -- degrees/sec

local M = {}

function M.on_start(self)
    lm.log("spin start: " .. self.name)
end

function M.on_update(self, dt)
    local t = self:get_transform()
    t.rotation.y = t.rotation.y + speed * dt  -- Y = yaw

    local mr = self:get_mesh_renderer()  -- nil-check: not auto-added
    if mr then
        local pulse = 0.5 + 0.5 * math.sin(lm.time.now() * 2.0)
        mr:set_color(0.35 + 0.6 * pulse, 0.45, 0.95 - 0.6 * pulse)
    end
end

return M
```

### World-space mover (WASD, no rotation)

Moves along world axes only — it ignores the entity's facing, so it's NOT a
first-person controller (for that see below). Fine for a top-down or fixed-camera
mover.

```lua
local speed = 6.0

local M = {}

function M.on_update(self, dt)
    local t = self:get_transform()  -- re-fetch every frame, never cache
    local dx, dz = 0.0, 0.0
    if lm.input.key_down("w") then dz = dz - 1 end
    if lm.input.key_down("s") then dz = dz + 1 end
    if lm.input.key_down("a") then dx = dx - 1 end
    if lm.input.key_down("d") then dx = dx + 1 end
    t.position.x = t.position.x + dx * speed * dt
    t.position.z = t.position.z + dz * speed * dt
    if dx ~= 0 or dz ~= 0 then lm.audio.event("step") end
end

return M
```

### First-person controller (mouse-look + WASD)

Drives the entity's Transform with the canonical basis from "Coordinate system &
camera math". Attach to the **primary Camera** entity (or a parent the camera is
childed to). Mouse-look + camera-relative movement, correct handedness.

```lua
local speed = 6.0
local sens  = 0.12       -- degrees per pixel of mouse movement

local M = {}

function M.on_start(self)
    lm.input.set_cursor_captured(true)   -- hide + lock cursor for mouse-look
end

function M.on_update(self, dt)
    local t = self:get_transform()       -- re-fetch every frame, never cache

    -- Esc releases / recaptures the cursor so the user can reach UI.
    if lm.input.key_down(256) then       -- 256 = GLFW_KEY_ESCAPE
        lm.input.set_cursor_captured(false)
    end

    -- Mouse-look. dx/dy are only non-zero while the cursor is captured.
    if lm.input.cursor_captured() then
        local dx, dy = lm.input.mouse_delta()
        local yaw   = t.rotation.y - dx * sens   -- drag right turns right
        local pitch = t.rotation.x - dy * sens   -- mouse up looks up
        if pitch >  89 then pitch =  89 end       -- clamp so we never flip
        if pitch < -89 then pitch = -89 end
        t:set_rotation(pitch, yaw, 0)
    end

    -- Camera-relative movement on the ground plane.
    local yaw   = math.rad(t.rotation.y)
    local fwdH  = vec3(-math.sin(yaw), 0, -math.cos(yaw))
    local right = vec3( math.cos(yaw), 0, -math.sin(yaw))

    local move = vec3(0, 0, 0)
    if lm.input.key_down("w") then move = move + fwdH  end
    if lm.input.key_down("s") then move = move - fwdH  end
    if lm.input.key_down("d") then move = move + right end
    if lm.input.key_down("a") then move = move - right end

    local len = math.sqrt(move.x*move.x + move.y*move.y + move.z*move.z)
    if len > 0 then
        move = move * (speed * dt / len)         -- normalize so diagonals aren't faster
        t.position.x = t.position.x + move.x
        t.position.z = t.position.z + move.z
        lm.audio.event("step")
    end
end

return M
```

### Procgen town spawn (deterministic)

```lua
local M = {}

function M.on_start(self)
    local town = lm.procgen.town{ seed = 1337, n = 32, tile_size = 2.5 }
    lm.log(("town: %d pieces, %d repairs"):format(#town.pieces, town.repairs))

    -- Terrain ground as a runtime mesh.
    local ground = lm.scene.create("ground")
    local key = lm.assets.add_mesh("town_terrain",
                                   lm.procgen.terrain_mesh(town.terrain))
    ground:add_component("MeshRenderer", { mesh = key })

    -- One entity per built piece.
    for i, piece in ipairs(town.pieces) do
        local e = lm.scene.create("piece_" .. i)
        local pkey = lm.assets.add_mesh("piece_" .. i, piece.mesh)
        e:add_component("MeshRenderer", { mesh = pkey })
        local t = e:get_transform()
        t:set_position(piece.anchor.x, piece.anchor.y, piece.anchor.z)
    end
end

return M
```

> Note: a `lm.scene.create` entity has only a `Name` (and a `Transform` once you
> call `get_transform`). Give it a renderer with
> `e:add_component("MeshRenderer", { mesh = key })` — no scene pre-authoring
> needed. Runtime meshes/textures persist across scene reloads (see `lm.assets`),
> so a one-time build in `on_start` is enough.
