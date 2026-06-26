---
name: liminal-lua
description: How to write Lua gameplay scripts for the liminal engine via the `lm` global — scene queries, input, time, audio, procgen, AI, and the entity script lifecycle. Use when creating or editing .lua files in a liminal project.
---

# liminal Lua scripting

liminal runs Lua 5.4 (via sol2). Each entity that has a `Script` component
points at a `.lua` file. The file is loaded in a fresh per-entity environment
and **must return a table** with optional `on_start` / `on_update` callbacks.
The `lm` global is the entire engine API surface.

## Script file shape (lifecycle)

A script returns a behavior table. The host calls:

- `on_start(self)` — once, the first frame the entity's script is live (and
  again after a hot reload). `self` is the owning `Entity`.
- `on_update(self, dt)` — every frame. `self` is the owning `Entity`, `dt` is
  the frame delta in **seconds** (a number passed by the engine — there is no
  `lm.time.dt`).

Anything else returned in the table is ignored by the engine but usable as your
own state/helpers. Locals declared at file scope are per-entity (fresh
environment per instance), so module-level config is fine.

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

Returning anything other than a table is an error. `on_start` / `on_update`
are both optional, but a script with neither does nothing.

**Hot reload:** in the editor (Play mode) the file is watched (~0.5s); saving
re-inits all instances of that script (`on_start` runs again). The shipped
player has hot reload off.

**Error handling:** every callback is pcall'd. Errors are logged once
(deduplicated) and the script keeps running next frame; fixing the file and
saving re-reports if it still errors.

## The Entity handle

`Entity` is a lightweight value handle (copyable). Methods:

- `self.name` — read/write the `Name` component (string property). Writing adds
  a `Name` if missing.
- `self:valid()` — is the entity still alive.
- `self:destroy()` — destroy this entity.
- `self:get_transform()` — returns the `Transform` (auto-adds one if missing, so
  this never returns nil on a valid entity).
- `self:get_mesh_renderer()` — returns the `MeshRenderer` or **nil** if absent
  (renderers are never auto-added; always nil-check).
- `self:get_component("Name")` — generic accessor for any registered component
  by name; returns the component or nil.
- `self:add_component("MeshRenderer", { mesh = "builtin:box" })` — add (or
  replace) a component by name from a table of fields; returns the new component
  handle (frame-local). The optional table is the same shape the component
  serializes to. This is how a script gives a freshly-created entity a
  `MeshRenderer` (or `Collider`, `Light`, …) — no scene pre-authoring needed.
- `self:remove_component("Light")` — remove a component by name (no-op if absent).
- `self:has_component("Collider")` → bool.

**Frame-local pointer caveat (important):** components returned by
`get_transform` / `get_mesh_renderer` / `get_component` are raw pointers into
the entt registry. They are valid only for the current frame and become
dangling after any structural change (add/remove component, create/destroy
entity). **Never cache a component across frames** — re-fetch it each
`on_update`.

## Components and their Lua fields

Fetch via the accessors above. Field names:

- **Name**: `.value` (string).
- **Transform**: `.position`, `.rotation`, `.scale` (all `vec3`; rotation is in
  **degrees**). Also `t:set_position(x,y,z)`, `t:set_rotation(x,y,z)`,
  `t:set_scale(x,y,z)`. Mutating `t.position.y = 2` mutates the real component
  (vecs return by reference).
  - **Euler order gotcha:** rotation is applied yaw→pitch→roll = **Y → X → Z**
    (`rotation.y` is yaw, `.x` is pitch, `.z` is roll). Spinning an object
    "around vertical" means changing `rotation.y`.
- **MeshRenderer**: `.mesh` (asset string), `.texture` (asset string),
  `.color` (`vec4`, gradient **bottom** tint), `.color2` (`vec4`, gradient
  **top** tint — blended up the mesh's local height). Also `mr:set_color(r,g,b)`
  / `mr:set_color(r,g,b,a)` (which **also** sets `color2` to the same value, so
  the object is a flat tint unless you set a gradient) and
  `mr:set_color2(r,g,b)` / `(r,g,b,a)` (the top color only — call after
  `set_color` for a base→top gradient). Mesh names:
  `builtin:box|pyramid|pillar|arch|stair|plane|quad`, seeded primitives like
  `builtin:blob:42` / `builtin:tree:7` / `builtin:rock:3` / `builtin:crystal:5`,
  the parametric `builtin:form:<sides>,<twist>,<taper>[,<seed>]` (an n-gon prism,
  sides 3..8, e.g. `builtin:form:6,0.2,0.5`), or a `runtime:` key from
  `lm.assets.add_mesh`.

- **Camera**: `.fov`, `.near`, `.far` (numbers), `.primary` (bool), `.shader`
  (string — the shader pack name; see the Shaders section). Fetch via
  `self:get_component("Camera")` (nil if absent).

- **Light**: `.color` (`vec3`), `.intensity` (number). Also
  `light:set_color(r,g,b)`. Fetch via `self:get_component("Light")`.

- **Collider**: `.center` (`vec3`), `.half_extents` (`vec3`) — an axis-aligned
  box for `lm.physics`. Zero `half_extents` means "derive from the mesh bounds"
  at query time.

- **Billboard**: `.yaw_only` (bool). Presence makes the entity rotate to face
  the active camera each frame — yaw-only when true, full-facing (also pitch)
  when false.

`AudioSource` and `Script` are registered components but expose no Lua usertype
fields beyond `get_component` returning nil/an opaque handle — drive audio/AI
through the `lm.*` namespaces below.

## Scene file format (`.lscene`)

A scene is a JSON document (`scenes/*.lscene`). **Prefer to build or edit scenes
with the in-editor MCP tools** (`create_entity`, `set_component`,
`destroy_entity`, then `save_scene`) — they always emit a valid document. Hand-
writing a `.lscene` is a fallback; if you do, you **must** include the top-level
`"liminal_scene": 1` key or the load fails with
`not a liminal_scene v1 document`.

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
- `id` is debug-only and is **not** preserved across a load: entities get fresh
  entt ids assigned in file order. Never rely on a specific id.
- `components` maps a registry component **name** → that component's JSON body.
- An unknown component name **warns on stderr and is skipped** (never fatal), so
  a typo'd component just silently vanishes from the loaded entity.
- The JSON field names are the **serialized** names, which differ from the Lua
  usertype field names above: `meshAsset`/`textureAsset` (not `.mesh`/`.texture`),
  `rotationEuler` (not `.rotation`), `fovDeg`/`nearZ`/`farZ` (not `.fov`/etc).
- `Transform.rotationEuler` is in **degrees**, applied yaw→pitch→roll = Y→X→Z
  (so `[pitch, yaw, roll]` order in the array is `x, y, z`).

**Component JSON bodies** (defaults shown; omit a field to take its default):
- `Name`: `{ "value": "crate" }`
- `Transform`: `{ "position": [0,0,0], "rotationEuler": [0,0,0], "scale": [1,1,1] }`
- `Camera`: `{ "fovDeg": 70.0, "nearZ": 0.1, "farZ": 220.0, "primary": true, "shaderName": "native" }`
- `MeshRenderer`: `{ "meshAsset": "builtin:box", "color": [1,1,1,1], "color2": [1,1,1,1], "textureAsset": "builtin:white" }`
  (`color` = gradient bottom tint, `color2` = top tint; omit `color2` to match
  `color`. `textureAsset` may be `""` for untextured.)
- `Light`: `{ "color": [1,1,1], "intensity": 1.0 }`
- `Script`: `{ "paths": ["scripts/foo.lua"] }` (one entity may list several scripts)
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
target in the standalone player and in editor Play; in editor Play pressing Stop
restores the original pre-Play scene (the swap is deferred only in the player).

## Math types

- `vec3(x,y,z)` (or `vec3()`, `vec3(s)`) — fields `.x .y .z`, supports `+ - *`
  (scalar), unary `-`, `:length()`, `tostring`.
- `vec4(x,y,z,w)` — fields `.x .y .z .w`.
- Available as `lm.vec3` / `lm.vec4` as well as bare `vec3` / `vec4`.
- Lua stdlib `math`, `string`, `table`, the **full `os`** (including
  `os.getenv/remove/rename/execute/exit/...`) and the **full `io`** library are
  open — scripts have complete standard-library file access. **Caveat:** `io`
  reads the real OS filesystem (via `resolve`/cwd), **not** the mounted pak, so
  in a shipped game an `io.open` cannot read a file packed into the `.pak` —
  read bundled text through the engine instead, or keep config inline. For
  structured data prefer `lm.json` (below); to load another script's table use
  `lm.import`.

## `lm` API reference

### lm.log(msg)
Print to the engine console. `lm.log("text")`.

### lm.json
- `lm.json.encode(value)` → JSON string. Lua tables encode as a JSON array when
  their keys are a contiguous `1..n` integer run, otherwise as an object.
  Functions/userdata become `null`.
- `lm.json.decode(string)` → Lua value (arrays as `1..n` tables). **Raises** a
  Lua error on malformed JSON — wrap in `pcall` if the source is untrusted (e.g.
  LLM output). Pairs naturally with `lm.ai` JSON specs.

### lm.import(path)
Project-relative module loader (the VFS-aware analogue of `require`): reads
`path` through the asset system (pak first, then the search paths), runs the
chunk **once** in the shared Lua state, and caches its return value. Every
`lm.import("scripts/util.lua")` from any entity script returns the **same**
cached table — this is the supported way to share code AND mutable state across
scripts (entity scripts otherwise each get an isolated environment). The module
file should `return` a table. Re-entrant imports of a file still loading get
`nil` (cycle guard); a read/compile/runtime error is logged and returns `nil`
(never throws).

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
- `lm.scene.change(path)` → request a scene swap (player only; a no-op warning
  in editor Play — the swap is deferred only in the standalone player).

### lm.input
- `lm.input.key_down(key)` → bool. `key` is a GLFW keycode (number) or a single
  character string (`"w"`, `"A"` — letters map to their uppercase ASCII code).
  Always false in windowless/headless hosts.
- `lm.input.mouse_down(button)` → bool. `button` is a GLFW mouse button
  (0 = left, 1 = right, 2 = middle).
- `lm.input.mouse_delta()` → `dx, dy` (two floats), accumulated cursor movement
  since the last call (zeroed on read). **Only non-zero while the cursor is
  captured** — call `lm.input.set_cursor_captured(true)` first or you get (0, 0).
- `lm.input.set_cursor_captured(captured)` — hide + lock the cursor for FPS-style
  mouse-look (true) or release it (false). Works in both the standalone player and
  editor Play mode. Typical pattern: capture in `on_start`, toggle with a key (e.g.
  Tab) so the user can reach UI. On Stop the editor releases the cursor for you.
- `lm.input.cursor_captured()` → bool, current capture state.

### lm.time
- `lm.time.now()` → seconds since the host started (monotonic, double).
- There is **no** `lm.time.dt` — use the `dt` argument to `on_update`.

### lm.ui  (immediate-mode 2D / HUD)
Screen-space drawing, re-issued every frame from `on_update` (immediate mode —
nothing persists; stop drawing and it disappears). Drawn into the scene
framebuffer, so it shows in the player, the editor viewport, and MCP
screenshots. Coordinates are **render-target pixels, origin top-left** (x→right,
y→down). Colors are 0..1 floats; alpha defaults to 1.
- `lm.ui.size()` → `w, h` — the current render-target pixel size. Use it to
  place/anchor HUD elements (for the `retro` pack this is the 400×300 virtual
  size, not the window).
- `lm.ui.rect(x, y, w, h, r, g, b [, a])` — filled rectangle. `lm.ui.quad` is an
  alias.
- `lm.ui.text(x, y, str, r, g, b [, a [, scale]])` — bitmap text (engine-baked
  8×8 font; `scale` multiplies the 8px cell, default 1). `x,y` is the top-left of
  the first glyph.
- `lm.ui.line(x0, y0, x1, y1, r, g, b [, a [, thickness]])` — line segment
  (default thickness 1).

```lua
function M.on_update(self, dt)
    local w, h = lm.ui.size()
    lm.ui.rect(0, 0, w, 18, 0, 0, 0, 0.5)          -- HUD bar
    lm.ui.text(6, 5, "SCORE " .. score, 1, 1, 1)   -- white text
    lm.ui.line(w/2 - 4, h/2, w/2 + 4, h/2, 1, 1, 1) -- crosshair
end
```

### lm.render  (per-frame render uniforms)
Drives the shared scene-shader knobs from Lua so a custom shader pack can react
per area / to a decay clock. Persist until changed (set them each frame, or once
when they change).
- `lm.render.set_fog(r, g, b, density)` — fog color + density (feeds `uFogColor`
  / `uFogDensity`).
- `lm.render.set_decay(p)` — a 0..1 "unravel" progress (feeds `uDecayProgress`).
- `lm.render.set_light(x, y, z)` — primary light direction (`uLightDir`).
- `lm.render.set_shade(x, y, z)` — secondary/disagreeing light (`uShadeDir`).
- `lm.render.set_uniform(name, number)` or `set_uniform(name, vec3)` — set an
  arbitrary named uniform on the active scene shader. Unknown names (not declared
  by the current pack) are silently ignored, so it is safe to set uniforms a pack
  may or may not read.

### lm.physics  (raycast / overlap)
World queries against entity boxes — each entity's `Collider` (if present and
non-degenerate) else its mesh's local bounds.
- `lm.physics.raycast(origin, dir [, maxDist])` → `{ entity, point, normal,
  distance }` or `nil`. `origin`/`dir` are `vec3` (`dir` is normalized for you);
  `maxDist` 0 or omitted = unbounded. `entity` is the hit `Entity`, `point`/
  `normal` are `vec3` in world space, `distance` is along `dir`.
- `lm.physics.overlap(a, b)` → bool — do the world-space AABBs of entities `a`
  and `b` intersect (false if either has no resolvable box).

```lua
-- eye ray pick
local t = self:get_transform()
local fwd = vec3(math.sin(t.rotation.y), 0, -math.cos(t.rotation.y))
local hit = lm.physics.raycast(t.position, fwd, 20.0)
if hit then lm.log("looking at " .. hit.entity.name) end
```

### lm.audio
The engine has one procedural DSP voice bank (not positional sources). The game
thread only pokes atomics; these calls are safe.
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
- `lm.assets.add_mesh(name, meshData)` → returns the resolvable storage key
  (a `runtime:` string) to put in `MeshRenderer.mesh`. `meshData` is a
  `MeshData` produced by procgen (e.g. `piece.mesh`, `lm.procgen.terrain_mesh`).
- `lm.assets.add_texture(name, path)` → loads an image file (PNG/JPEG/TGA/BMP)
  through the VFS and returns its `runtime:` key (put it in `MeshRenderer.texture`).
- `lm.assets.add_texture(name, w, h, pixels)` → builds a `w×h` RGBA8 texture from
  a flat Lua array `pixels` (length `w*h*4`, values 0..255, row-major, top-left
  origin); returns the `runtime:` key. For procedural / pixel-art textures.
- **Persistence:** runtime meshes/textures live for the asset cache's lifetime —
  they **survive** scene reloads, play/stop, and `lm.scene.change` (the cache is
  not rebuilt on those paths). They are lost only when the project is closed or
  the process exits. Re-registering in `on_start` is safe (idempotent overwrite)
  but not required for a key to resolve after a reload.
- **No texture hot-reload:** a texture file is decoded **once** into a GPU
  resource and cached for the asset cache's lifetime. Custom shaders hot-reload
  ~0.5s after you save them; textures do **not**. Editing a texture file on disk
  does **not** refresh what's already on screen — neither a scene reload,
  play/stop, `lm.scene.change`, nor the `reload_scene` MCP tool rebuilds the
  cache (they rebuild only the Scene + ScriptHost). To pick up a changed texture
  file: re-call `lm.assets.add_texture(name, path)` with the **same** `name`
  (overwrites the cache entry), use a fresh runtime name, or close + reopen the
  project (the only path that destroys the cache).

### lm.ai  (only when built with `LIMINAL_WITH_INFERENCE`)
Local LLM inference. The table may be absent — feature-test with `if lm.ai then`.
The engine pointer may also be null (tests / opted-out host): calls warn once and
return sensible defaults.
- `lm.ai.start{ model = "path", context = N, threads = N, gpu_layers = N }` —
  load a model (`model` is required, resolved through Assets).
- `lm.ai.stop()`.
- `lm.ai.status()` → `status, message` (status is `"stopped"|"loading"|"ready"|"failed"`).
- `lm.ai.submit{ system=, user=, grammar=, temperature=, top_p=, top_k=,
  repeat_penalty=, max_tokens=, seed= }` → request id (integer).
- `lm.ai.poll(id)` → `{ text=, complete=, failed=, error= }`.
- `lm.ai.cancel(id)`, `lm.ai.busy()` → bool, `lm.ai.queue_depth()` → int.
- `lm.ai.forget(id)` — **required** after a completed request. Finished
  responses are retained until forgotten; a script that never forgets leaks
  responses for the engine's lifetime.

### lm.procgen
Deterministic procedural generation. **Hard rule:** every entry point takes an
explicit seed (or an `Rng`); all randomness flows through these — never use Lua
`math.random` for world generation, or builds will be non-deterministic.

Primitives:
- `lm.procgen.rng(seed)` → `Rng` (xorshift32). `rng:next()` (uint32),
  `rng:next01()` (float 0..1), `rng:range(lo, hi)`.
- `lm.procgen.tileset(jsonPath?)` → `TileSet` (default set if no path). Methods:
  `:count()`, `:name(id)`, `:id_of(name)`, `:role_id(role)`, `:walkable(id)`.

Terrain:
- `lm.procgen.terrain{ seed=, kind=, water=, n=, tile_size= }` → `HeightField`.
  `kind`: `plane|hills|canyon|islands|flooded|void`. `water`: `none|some|lots`.
  `HeightField`: `.nodes`, `.cell`, `.half`, `.has_water`, `.water_level`,
  `:tile_height(x,z)`, `:tile_underwater(x,z)` (these take **tile/node
  indices**), and the **world-space** queries `:height_at_world(worldX, worldZ)`
  → terrain height (bilinear) and `:walkable_at_world(worldX, worldZ)` → bool
  (true when that point is not underwater). Use the world-space pair to place
  objects / the player on the ground without deriving the tile transform.
- `lm.procgen.terrain_mask(hf, params, tileset)` → opaque tile masks (params
  mirror the `terrain{}` fields).
- `lm.procgen.terrain_mesh(hf)` / `lm.procgen.water_mesh(hf)` → `MeshData`
  (hand to `lm.assets.add_mesh`).

WFC layout:
- `lm.procgen.stamp_footprint(masks, tileset, n, cx, cz, w, d)` → `FootprintPlan`
  or nil (claims a w×d building footprint).
- `lm.procgen.solve_wfc{ tileset=, masks=, n=, seed=, max_restarts= }` →
  `WfcResult` (`.solved`, `.restarts`).
- `lm.procgen.grid_from(wfcResult, n, tileSize)` → `TileGrid`
  (`.n`, `.tile_size`, `:in_bounds(x,z)`, `:at(x,z)`, `:set(x,z,id)`,
  `:center(x,z)` → `worldX, worldZ`).
- `lm.procgen.validate(grid, hf, sites, plans, tileset)` →
  `{ repairs=, unreachable_zones=, plans= }`. `sites` is an array of `{x=,z=}`
  pairs; `plans` an array of `FootprintPlan`. Mutates `grid`; returns
  door-filled plans. Never fails (always repairs).

Architecture:
- `lm.procgen.build_building(grid, hf, footprintPlan, familyParams, rng)` →
  `BuiltPiece`.
- `lm.procgen.collect_runs(grid, tileset)` → array of `DeckRun`
  (`.chain_len`, `.is_pier`).
- `lm.procgen.build_deck(grid, hf, tileset, deckRun)` → `BuiltPiece`.
- `FootprintPlan{ zone=, style=, x0=, z0=, w=, d=, door_x=, door_z= }`,
  `FamilyParams{ floor_height=, wall_thickness=, foundation=, min_floors=,
  max_floors=, roof=, window_*=, door_*=, inset_per_floor=, ruin_chaos=,
  colonnade=, stairs=, lamp_chance= }` (constructed from a table).
- `BuiltPiece`: `.mesh` (`MeshData`), `.zone`, `.anchor` (`vec3`),
  `:vertex_count()`, `:door_count()`, `:lamp_count()`, `:lamps()` (array of vec3).

One-shot:
- `lm.procgen.town{ seed=, n=, tile_size=, terrain=, water=, tileset=,
  families= }` → `{ grid=, terrain=, pieces=, repairs=, restarts= }`. Runs the
  full terrain→mask→WFC→validate→architecture pipeline deterministically.
  `pieces` is an array of `BuiltPiece`.

## Shaders

The renderer has a named shader-pack registry. Two built-ins are always
available: **`native`** (the default — clean, perspective-correct, crisp;
no texture warp) and **`retro`** (the PS1 look — low-res FBO, vertex snap,
affine UVs, fog). A `Camera` picks its pack by name via its `shaderName`
field, which shows up as a **dropdown in the Camera component inspector**.

### Authoring custom shaders

Drop GLSL under `<projectDir>/shaders/`. Two layouts, both auto-discovered on
project open (and **hot-reloaded ~0.5s after you save** — a compile error is
logged to the console and the previous working shader is kept). Both appear in
the Camera shader dropdown by name. Custom shaders render at native (window)
resolution.

**Frag-only — `shaders/<name>.frag`** (name in the dropdown = the file stem).
This file is a fragment **BODY ONLY**: no `#version`, no `in`/`out`/`uniform`
declarations. The engine prepends them and supplies the vertex stage. You write
just `void main(){ ... }`. Available to your body:

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
// body-only frag shader — engine supplies version/ins/uniforms
void main() {
    vec3 c = texture(uTex, vUV).rgb * mix(uColor, uColor2, vGradT);
    float g = dot(c, vec3(0.299, 0.587, 0.114));
    float lambert = max(dot(normalize(vNormal), normalize(uLightDir)), 0.0);
    FragColor = vec4(vec3(g) * (0.35 + 0.65 * lambert), 1.0);
}
```

**Full pack — `shaders/<name>/scene.vert` + `scene.frag`** (name = dir name).
You own both stages. Vertex attributes are location `0 = pos (vec3)`,
`1 = normal (vec3)`, `2 = uv (vec2)`. You may read any per-draw uniform the
renderer sets: `uModel, uView, uProj, uNormalMat, uColor, uColor2, uGradBase,
uGradInv, uTex, uLightDir` (plus retro-only ones — unused uniforms are
harmless). The blit/upscale stage is engine-provided.

### Switching a camera's shader at runtime

The `Camera` component is Lua-bound. Get it with `get_component` and set
`.shader` (also `.fov`, `.near`, `.far`, `.primary`):

```lua
local cam = self:get_component("Camera")
if cam then cam.shader = "grayscale" end  -- or "native" / "retro" / a full pack
```

(`get_component` returns a frame-local pointer — set it in the frame you fetch
it, don't cache it.)

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

### Input-driven mover (WASD)

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
> `e:add_component("MeshRenderer", { mesh = key })` — no scene pre-authoring or
> entity pool needed. Runtime meshes/textures persist across scene reloads (see
> `lm.assets`), so a one-time build in `on_start` is enough.
