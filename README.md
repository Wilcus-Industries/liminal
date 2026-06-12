# liminal

Liminal is a small C++20 game engine extracted from the game *Grey Matter*: OpenGL rendering (4.1 core on macOS, 4.6 elsewhere) over GLFW/glad, Dear ImGui (docking), GLM math, EnTT ECS, miniaudio, nlohmann/json, optional Lua 5.4 + sol2 scripting (`LIMINAL_WITH_SCRIPTING`), and optional local LLM inference via llama.cpp with Metal + Accelerate on Apple (`LIMINAL_WITH_INFERENCE`). All dependencies are fetched at configure time via CMake FetchContent.

## Build

```sh
cmake -B build -G Ninja
cmake --build build
```

The glad OpenGL loader is generated at configure time by a Python tool that requires the `jinja2` package. Plain `python3` is tried first; if it lacks jinja2, point CMake at any interpreter that has it:

```sh
cmake -B build -G Ninja -DPython_EXECUTABLE=/path/to/venv/bin/python
```

Notes:

- The first configure downloads all dependencies (including llama.cpp) and the first build compiles llama.cpp's Metal kernels — expect it to take a while.
- Options: `LIMINAL_WITH_INFERENCE` (ON), `LIMINAL_WITH_SCRIPTING` (ON), `LIMINAL_BUILD_EDITOR` and `LIMINAL_BUILD_EXAMPLES` (ON when top-level).
- Run the example: `./build/examples/01_window/01_window` (ESC quits).
