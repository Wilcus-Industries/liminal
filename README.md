# LIMINAL

Liminal is a C++20 scriptable game engine local LLM inference using llama.cpp.

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
