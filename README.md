# liminal

A C++20 game engine for small, strange worlds: retro (PS1-flavored) OpenGL
rendering, an EnTT-backed scene with JSON serialization, Lua scripting,
procedural generation (WFC, terrain, shape grammar), procedural-DSP audio,
and local LLM inference via llama.cpp.

liminal is **editor-first**: you open a project folder in `liminal-editor`,
author scenes and Lua scripts, and ship a standalone executable with one
click. The engine itself is a static library consumed only by the editor and
the player — there is no public C++ framework surface to write `main()`
against.

## Modules

| Module      | Headers                  | What it does |
|-------------|--------------------------|--------------|
| `core`      | `liminal/core/`          | `App` loop, `Window` (GLFW), asset path resolution, `AssetCache`, pak archive + VFS |
| `render`    | `liminal/render/`        | Retro renderer (low-res FBO, fog, dithering), `Mesh` primitives, `Shader`, `Texture`; default shaders are embedded in the binary |
| `scene`     | `liminal/scene/`         | `Scene`/`Entity` facade over `entt::registry`, built-in components, `.lscene` JSON save/load, `ComponentRegistry` |
| `script`    | `liminal/script/`        | Lua 5.4 + sol2 `ScriptHost`, `lm.*` API, per-entity script environments, hot reload |
| `inference` | `liminal/inference/`     | llama.cpp wrapper: background worker, token streaming, GBNF grammars |
| `procgen`   | `liminal/procgen/`       | Tile vocabulary (`TileSet`), terrain, Wave Function Collapse, layout validator with repair, shape-grammar architecture, landmark structures |
| `audio`     | `liminal/audio/`         | miniaudio device + procedural DSP voice bank |
| `ui`        | `liminal/ui/`            | Dear ImGui layer (docking branch) |

## Build the toolchain

```sh
cmake -B build && cmake --build build -j
ctest --test-dir build        # headless determinism + round-trip tests
```

This produces two binaries side by side:

- `build/editor/liminal-editor` — the authoring environment.
- `build/player/liminal-player` — the standalone runtime that shipped games
  are built from (copied next to the editor).

### Build requirements

- CMake ≥ 3.24, Ninja recommended, a C++20 compiler.
- Python with the `jinja2` package — the glad OpenGL loader is generated at
  configure time. Plain `python3` is tried first; otherwise point CMake at an
  interpreter that has it:

```sh
cmake -B build -G Ninja -DPython_EXECUTABLE=/path/to/venv/bin/python
```

The first configure downloads all dependencies (including llama.cpp); the
first build compiles llama.cpp's Metal kernels — expect it to take a while.

## Options

| Option                   | Default          | Effect |
|--------------------------|------------------|--------|
| `LIMINAL_WITH_INFERENCE` | `ON`             | llama.cpp local inference (`liminal/inference/engine.hpp`) |
| `LIMINAL_BUILD_EDITOR`   | ON when top-level| `liminal-editor` (pulls ImGuizmo) |
| `LIMINAL_BUILD_PLAYER`   | ON when top-level| `liminal-player` standalone runtime |
| `LIMINAL_BUILD_TESTS`    | ON when top-level| headless ctest suite |

## Authoring a game

```sh
./build/editor/liminal-editor [path/to/project.ljson]
```

Open a **project folder** (containing a `project.ljson`) in the editor. From
there you get a dockable scene editor — hierarchy, registry-driven inspector,
ImGuizmo translate/rotate/scale (W/E/R, F to frame), `.lscene` open/save, a
tabbed script/text editor with Lua completion, and play-in-editor with script
hot reload. `editor/sample_project/` is a ready project to poke at.

### Scripts

A script is a Lua file that returns a behavior table. Attach it to an entity
via a `Script` component (set its path in the inspector). Each instance runs in
its own environment (globals readable, writes stay local). Two optional hooks:

```lua
local M = {}

function M.on_start(self)            -- once, when the scene / Play begins
    lm.log("hello from " .. self.name)
end

function M.on_update(self, dt)        -- every frame; dt = seconds since last
    local t = self:get_transform()
    t.rotation.y = t.rotation.y + 90.0 * dt
end

return M
```

`self` is the owning entity. Both hooks are `pcall`'d — an error is logged
(deduped) and won't crash the game. In the editor, scripts hot-reload on a
0.5 s mtime watch while in Play mode.

**Entity API** (`self` and anything from `lm.scene`):

| Call | Result |
|------|--------|
| `self.name` | get / set the `Name` string |
| `self:valid()` / `self:destroy()` | liveness / kill |
| `self:get_transform()` | `Transform` (auto-added if missing) |
| `self:get_mesh_renderer()` | `MeshRenderer`, or `nil` if absent |
| `self:get_component("X")` | raw component pointer, or `nil` |

`Transform` exposes `.position`, `.rotation` (euler degrees, Y→X→Z), `.scale`
as mutable-by-reference `vec3`s, plus `:set_position/rotation/scale(x,y,z)`.
`MeshRenderer` exposes `.mesh`, `.texture`, `.color`, and `:set_color(r,g,b,a)`.
Component pointers are valid for the current frame only — never cache them
across frames.

### The `lm` global

| Namespace     | What it exposes |
|---------------|-----------------|
| `lm.log` / `lm.vec3` / `lm.vec4` | print to the console; vector constructors (`+ - *`, `:length()`) |
| `lm.scene`    | `find` / `find_all` / `create` / `each` / `destroy` entities, `change` (scene swap — player only) |
| `lm.input`    | `key_down(key)` — GLFW keycode or single-char string |
| `lm.time`     | `now()` — seconds elapsed (per-frame delta is the `dt` arg to `on_update`) |
| `lm.audio`    | `set` / `get` voice params, `event` (one-shot: step/jump/mumble/door_creak), `ok` |
| `lm.ai`       | local LLM: `start` / `submit` / `poll` / `cancel` / `forget` / `status` (gated on `LIMINAL_WITH_INFERENCE`; feature-test `if lm.ai then`) |
| `lm.procgen`  | the procgen pipeline: terrain, WFC, shape grammar, plus a one-shot `town{}` |
| `lm.assets`   | `add_mesh` — register a runtime mesh |

## Shipping a standalone game

In the editor, use **Game → Build Game…**. The editor packs the project's
scenes, scripts, and assets into a `.pak` archive and produces a single
self-contained executable based on `liminal-player`:

- **Windows / Linux:** the pak is appended to a copy of the player binary —
  one file, no sidecar.
- **macOS:** the executable ships with a sidecar `.pak` next to it (codesigning
  rejects a Mach-O with data appended after the load commands).

Any `.gguf` inference models referenced by the project are copied beside the
shipped executable. Launching the player locates its pak (embedded in its own
binary, or the sidecar), mounts it as the virtual filesystem, and runs the
game with hot reload off.

## Version & license

Current version: **0.1.0** (`liminal::version()`, `<liminal/version.hpp>`).
MIT — see [LICENSE](LICENSE).
