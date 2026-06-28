<div align="center">

<img src="assets/liminal_icon_dark.png" alt="Liminal" width="160" />

# LIMINAL

The game engine for agents.

<img src="assets/liminal_app.png" alt="Liminal editor" width="800" />

</div>

## Headless Mode

Liminal can be run in headless mode, without a user interface. This allows the agent to develop games completely without any graphical interface.

Your agent will be able to take screenshots, and interact with the normal editor buttons and panels, just like in the regular editor, without any graphical interface.

This is useful for running the agent on a GUI-less server or in a CI/CD pipeline.

## Compatible With

- Claude, Codex, any agent with MCP+skill support.
- MacOS, Linux, Windows. User interface not even needed through headless mode.

## Features

- Full-fledged editor.
- LLM inference built in (GGUF models).
- Procedural generation built in. (Wave function collapse, terrain, shape-grammar architecture, etc.)
- Audio with procedural DSP voice bank.
- Fully static shipping builds.
- Built in Claude Code support in the editor (MCP workspace, Lua `lm` library skill for scripting)
- ECS system built in (EnTT).

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
