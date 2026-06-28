<div align="center">

<img src="assets/liminal_icon_dark.png" alt="Liminal" width="160" />

# LIMINAL

The game engine for agents.

<img src="assets/liminal_app.png" alt="Liminal editor" width="800" />

</div>

## Compatible With

- Claude, Codex, any agent with MCP+skill support.
- MacOS, Linux, Windows. User interface not even needed through headless mode.

## Features

- Full-fledged editor.
- Headless mode for agentic development on no-GUI environments.
- Procedural generation built in. (Wave function collapse, terrain, shape-grammar architecture, etc.)
- LLM inference built in (GGUF models).
- Audio with procedural DSP voice bank.

## Installing

Built binaries currently only available for MacOS on [GitHub Releases](https://github.com/Wilcus-Industries/liminal/releases).

After installing, it is recommended to run the `--install-skill` command if you will be running Liminal headlessly. This sets up the project bootstrapping skill for the agent.

```sh
# Wherever you installed Liminal to.
~/Applications/Liminal.app/Contents/MacOS/liminal-editor --install-skill
```

Windows builds will be available soon. Linux builds will be available for major distributions soon.

## Building

Requires CMake 3.20+ and a C++20 compiler.

### Mac

```sh
xcode-select --install
cmake -B build
cmake --build build -j
```

### Linux

```sh
sudo apt install build-essential cmake libgl1-mesa-dev xorg-dev
cmake -B build
cmake --build build -j
```

### Windows

```sh
cmake -B build
cmake --build build --config Release -j
```

## Changelog

### v0.2.2

- Headless mode: run the editor with no GUI, driven entirely over MCP — plus a display-less offscreen rendering backend (EGL/OSMesa) for bare CI/SSH boxes with no display server.
- Agent bootstrap skill + global MCP auto-connect, so a fresh agent in any directory can discover and launch Liminal.
- Agent synthetic input: an agent can now "play" a running game by injecting keyboard/mouse input over MCP.
