# liminal

A C++20 game engine for small, strange worlds: retro (PS1-flavored) OpenGL
rendering, an EnTT-backed scene with JSON serialization, Lua scripting,
procedural generation (WFC, terrain, shape grammar), procedural-DSP audio,
and local LLM inference via llama.cpp — all statically linked, no runtime
asset files required by the library itself.

## Modules

| Module      | Headers                  | What it does |
|-------------|--------------------------|--------------|
| `core`      | `liminal/core/`          | `App` loop, `Window` (GLFW), asset path resolution, `AssetCache` |
| `render`    | `liminal/render/`        | Retro renderer (low-res FBO, fog, dithering), `Mesh` primitives, `Shader`, `Texture`; default shaders are embedded in the binary |
| `scene`     | `liminal/scene/`         | `Scene`/`Entity` facade over `entt::registry`, built-in components, `.lscene` JSON save/load, `ComponentRegistry` |
| `script`    | `liminal/script/`        | Lua 5.4 + sol2 `ScriptHost`, `lm.*` API, per-entity script environments, hot reload |
| `inference` | `liminal/inference/`     | llama.cpp wrapper: background worker, token streaming, GBNF grammars |
| `procgen`   | `liminal/procgen/`       | Tile vocabulary (`TileSet`), terrain, Wave Function Collapse, layout validator with repair, shape-grammar architecture, landmark structures |
| `audio`     | `liminal/audio/`         | miniaudio device + procedural DSP voice bank |
| `ui`        | `liminal/ui/`            | Dear ImGui layer (docking branch) |

## Quickstart (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(liminal
    GIT_REPOSITORY https://github.com/you/liminal   # or a local path
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(liminal)

target_link_libraries(my_game PRIVATE liminal::liminal)
```

```cpp
#include <liminal/core/app.hpp>
#include <liminal/scene/components.hpp>

int main() {
    liminal::App app({.title = "hello"});
    auto crate = app.scene().create("crate");
    crate.add<liminal::Transform>({.position = {0, 1, -4}});
    crate.add<liminal::MeshRenderer>({.meshAsset = "builtin:box"});
    crate.add<liminal::Script>({.path = "scripts/spin.lua"});
    app.run(nullptr); // built-in scene render + script update
}
```

```lua
-- scripts/spin.lua — the script contract: return a table with hooks.
local M = {}
function M.on_update(self, dt)
    local t = self:get_transform()
    t.rotation.y = t.rotation.y + 90.0 * dt
end
return M
```

## Build requirements

- CMake ≥ 3.24, Ninja recommended, a C++20 compiler.
- Python with the `jinja2` package — the glad OpenGL loader is generated at
  configure time. Plain `python3` is tried first; otherwise point CMake at an
  interpreter that has it:

```sh
cmake -B build -G Ninja -DPython_EXECUTABLE=/path/to/venv/bin/python
cmake --build build
ctest --test-dir build        # headless determinism + .lscene round-trip tests
```

The first configure downloads all dependencies (including llama.cpp); the
first build compiles llama.cpp's Metal kernels — expect it to take a while.

## Options

| Option                   | Default          | Effect |
|--------------------------|------------------|--------|
| `LIMINAL_WITH_INFERENCE` | `ON`             | llama.cpp local inference (`liminal/inference/engine.hpp`) |
| `LIMINAL_WITH_SCRIPTING` | `ON`             | Lua 5.4 + sol2 scripting (`liminal/script/script_host.hpp`) |
| `LIMINAL_BUILD_EDITOR`   | ON when top-level| `liminal-editor` (pulls ImGuizmo) |
| `LIMINAL_BUILD_EXAMPLES` | ON when top-level| the `examples/` programs |
| `LIMINAL_BUILD_TESTS`    | ON when top-level| headless ctest suite |

`LIMINAL_WITH_*` are PUBLIC compile definitions — consumers see the same
feature set the library was built with.

## Editor

```sh
./build/editor/liminal-editor [scene.lscene]
```

Dockable scene editor: hierarchy, registry-driven inspector, ImGuizmo
translate/rotate/scale (W/E/R, F to frame), `.lscene` open/save, and
play-in-editor with script hot reload. `editor/sample_project/` is a ready
scene to poke at.

## Examples

| Example        | Shows |
|----------------|-------|
| `01_window`    | minimal `Window` + GL clear loop |
| `02_retro_cube`| retro renderer, mesh primitives, fog/dither look |
| `03_scene_lua` | Lua scripts on entities, hot reload |
| `04_wfc_town`  | full procgen pipeline: terrain → WFC → validator → shape grammar |
| `05_scene`     | `.lscene` authoring, save/load round-trip |

Run from the repo root, e.g. `./build/examples/01_window/01_window` (ESC quits).

## Installing / find_package

```sh
cmake --install build --prefix /opt/liminal
```

The installed package is self-contained: every FetchContent dependency (glm,
nlohmann_json, EnTT, sol2 + Lua, imgui, glfw, glad, stb, miniaudio,
llama.cpp) is compiled in or installed alongside and exported under the
`liminal::` namespace. Consumers need only:

```cmake
find_package(liminal 0.1 CONFIG REQUIRED)   # CMAKE_PREFIX_PATH=/opt/liminal
target_link_libraries(my_game PRIVATE liminal::liminal)
```

Limitations: the install ships static libraries for one platform/arch as
built; third-party headers land under namespaced dirs (`liminal-imgui/`,
`liminal-lua/`, ...) plus standard ones (`glm/`, `nlohmann/`, `entt/`,
`sol/`, `GLFW/`). For day-to-day development the FetchContent path above is
the primary, most-tested route.

## Version & license

Current version: **0.1.0** (`liminal::version()`, `<liminal/version.hpp>`).
MIT — see [LICENSE](LICENSE).
