# liminal

Small C++20 game engine with a switchable renderer (clean perspective-correct "native" look by default; the PS1-style "retro" pack is one per-camera shader pick away), ECS scenes (entt), Lua scripting, procedural generation (WFC towns, terrain, shape grammar), optional local LLM inference (llama.cpp), and a dockable ImGui editor with play-in-editor.

**Editor-only workflow.** The `liminal` static lib is internal — consumed only by the editor and the standalone player; there is no public C++ framework surface and no `find_package(liminal)` install/export. You open a project (`project.ljson`) in `liminal-editor`, script everything via the `lm` Lua global, and ship a standalone game via the editor's "Build Game…" (Game menu) which packs a `.pak` and produces a single executable based on `liminal-player`.

## Maintenance rule

**Update this CLAUDE.md after every modification to the project** — new modules, renamed files, changed CMake options, new examples/tests, or architecture changes. Keep the structure section and module notes in sync with the code.

## Build

```sh
cmake -B build
cmake --build build -j
ctest --test-dir build        # run tests
```

CMake options (root `CMakeLists.txt`):

| Option | Default | Effect |
|---|---|---|
| `LIMINAL_WITH_INFERENCE` | ON | llama.cpp local inference (`src/inference`) |
| `LIMINAL_BUILD_EDITOR` | top-level | Editor (pulls ImGuizmo) |
| `LIMINAL_BUILD_PLAYER` | top-level | Standalone game player (`liminal-player`) |
| `LIMINAL_BUILD_TESTS` | top-level | Headless ctest tests |

No install/export rules — liminal is no longer a `find_package` consumable; the lib is internal to the editor/player builds.

Dependencies fetched via `cmake/Dependencies.cmake` (FetchContent): GLFW, glad, glm, entt, Dear ImGui, miniaudio, stb, nlohmann/json, sol2+Lua, llama.cpp, ImGuizmo, ImGuiColorTextEdit (BalazsJako, editor-only, compiled into the editor like ImGuizmo), libvterm (neovim mirror, editor-only; plain C core `src/*.c` + `include/vterm.h`, no usable CMake so the `.c` files compile straight into liminal-editor like ImGuizmo — include both `include/` and `src/`; pinned commit `934bc2fbf21800ac3458a499df8820ca5fb45fd3`), cpp-httplib (yhirose, editor-only, header-only; INTERFACE target `httplib::httplib` carries the include dir + Threads dep — backs the editor's MCP server; plaintext localhost only, OpenSSL/zlib/brotli forced off; pinned `v0.18.3`), JetBrains Mono (editor-only; plain TTF zip, path exported as `LIMINAL_JETBRAINS_MONO_TTF`).

## Structure

```
include/liminal/          public headers (mirrors src/), umbrella: liminal.hpp
src/
  core/      App (main loop, owns everything; ctor takes a ScriptContext, not a
             Window*), Window (GLFW+glad RAII, input), Assets (search-path resolution
             + VFS: readFile / mountPak read through a mounted pak first), AssetCache
             (name → GPU resource, "builtin:"-prefixed procedural meshes/textures,
             warn-once failures; addMesh returns the stored "runtime:" key),
             pak (LMPK\0v01-footer archive: buildGamePak helper packs project files
             + synthesizes project.ljson carrying width/height through from the
             source; PakWriter::add drops paths > 65535 bytes; player reads it),
             platform (selfExePath/selfExeDir; openUrl — OS default handler via
             open/xdg-open/ShellExecute, used by the editor landing-screen repo
             link; userConfigDir — ~/.liminal or %APPDATA%/liminal, created on
             first use, backs the editor recent-projects file),
             pty (liminal::Pty: RAII pseudo-terminal — forkpty on macOS/Linux,
             TERM=xterm-256color child env, optional cwd arg (child chdir()s
             before exec, non-fatal on fail), O_NONBLOCK master, read/write/
             resize/alive; Windows stubbed pending ConPTY. Holds no locks/threads —
             owner serializes; safe for one reader thread + main-thread writer.
             Used by the editor terminal panel)
  render/    Renderer (named shader-pack registry, two-pass: scene → render FBO →
             nearest-neighbor blit/upscale. Two built-in packs, both registered by
             the ctor (default ctor + player default to "native"):
               "native" — default, CRISP. Perspective-correct UVs, single lambert +
                           ambient, no vertex snap / no affine / no fog. Renders into a
                           WINDOW-resolution FBO (ShaderResolution::Native), so no
                           pixelation and no texture warp.
               "retro"  — the PS1 look. Renders into the 400x300 virtual FBO
                           (ShaderResolution::LowRes) + nearest upscale; vertex snap,
                           affine UVs and near fog. The nearest-filtering / low-res
                           note below is scoped to THIS pack.
             ShaderPack = 4 GLSL strings + label + `res` (ShaderResolution
             Native|LowRes), with static standard() (native, embedded), retro()
             (embedded), fromFiles(), makeFullPack(vert,frag) and
             makeFragOnlyPack(fragBody) (wraps a body-only fragment with the native
             frag header + native vertex stage; reuse the shared blit). The blit
             (upscale) stage is shared by both packs.
             Registry API: registerShaderPack(name,pack) (lazy-compiles; recompiles
             immediately if it's the active pack = hot reload), useShaderPack(name)
             (cheap to call every frame: no-op+true if already active, false +
             keeps current on unknown name / compile failure), availableShaderPacks(),
             activeShaderPack(); legacy setShaderPack() retained (reserved
             "__explicit__" name). Per-shader FBO resolution: Native uses window fb
             size, LowRes uses settings.virtualW/H — so beginFrame's signature is
             beginFrame(view, windowFbWidth, windowFbHeight). Process-global
             std::vector<std::string>& shaderCatalog() lists selectable pack names
             for UI (filled by editor/player after registering; renderer never reads
             it). colorTexture() getter for editor; readPixels(rgba,w,h) reads the
             color FBO back to RGBA8 bottom-up — backs the MCP screenshot tool),
             Shader (throws on compile/link
             fail), Mesh (interleaved pos|normal|uv, flat-shaded, no index buffer,
             procedural primitives), Texture (procedural RGBA8 + stb_image
             decoding PNG/JPEG/TGA/BMP — the formats buildGamePak ships; nearest
             filtering is load-bearing for the retro look. Min filter is
             GL_NEAREST_MIPMAP_NEAREST + glGenerateMipmap after upload (mag
             stays GL_NEAREST) — still point-sampled, never linear, but mip
             selection stops large textures thrashing the GPU cache on
             minification (was an FPS cliff under plain GL_NEAREST min))
  scene/     Scene (facade over entt::registry; Entity = {entt::entity, Scene*} value
             handle), components (Name, Transform, MeshRenderer, Camera (now carries
             shaderName, default "native"; serialized via j.value(..,"native"),
             inspector shows a dropdown over shaderCatalog() (free-text InputText
             fallback when the catalog is empty), Lua-bound — Camera usertype exposes
             fov/near/far/primary/shader so scripts can switch the per-camera shader
             at runtime), AudioSource,
             Light, Script (holds a `paths` vector — an entity may run multiple
             scripts; legacy single-`path` JSON still loads via a fromJson fallback);
             Transform euler order yaw→pitch→roll = Y→X→Z),
             ComponentRegistry (name → toJson/fromJson/inspect/Lua-bind, singleton),
             serialize (.lscene JSON v1; entities sorted by entt id for stable output;
             unknown components warn+skip; entity IDs NOT preserved across load)
  script/    ScriptHost (one sol::state, one sol::environment per (entity,path)
             instance — multiple scripts per entity, m_instances keyed
             entity → vector<Instance>; on_start/on_update; pcall error dedup;
             hot reload via 0.5s mtime watch → re-init every instance sharing the
             changed path; setErrorSink routes [lua] errors and setLogSink routes
             lm.log output to a host sink — both always print to stdout/stderr too,
             the editor wires both into its console),
             lua_bindings (glm vecs, Entity usertype, get_component pointers frame-local
             only; the `lm` global is wired from a ScriptContext{input, audio, assets,
             inference, hotReload, requestSceneChange} that replaced the old Window* ctor).
             lm.* API surface:
               lm.scene   — find / create / find_all / each / destroy / change (scene swap)
               lm.input   — key_down (char or GLFW keycode) / mouse_down /
                            mouse_delta (accumulated cursor delta, zeroed on read) /
                            set_cursor_captured / cursor_captured (mouse-look lock)
               lm.time    — now (no dt binding; dt is the on_update(self, dt) arg)
               lm.audio   — set / get / event / ok
               lm.ai      — start / stop / status / submit / poll / cancel / forget /
                            busy / queue_depth (gated LIMINAL_WITH_INFERENCE)
               lm.procgen — full pipeline (lua_bindings_procgen.cpp): rng / terrain /
                            terrain_mask / stamp_footprint / solve_wfc / grid_from /
                            validate / build_building / collect_runs / build_deck /
                            terrain_mesh / water_mesh, plus the one-shot town{seed=…}
               lm.assets  — add_mesh (register a runtime: mesh)
  procgen/   rng (deterministic xorshift32, per-stage salted seeding), types (plain
             structs between stages), wfc (tiled-model WFC, weighted picks, restart on
             contradiction with seed+1), tileset (JSON overlay on compiled-in fallback),
             terrain/terrain_field (value noise, quantized, water/void masks),
             shape_grammar (footprints → boxy buildings/decks/piers), structure
             (parameterized landmarks), layout_validator (flood-fill reachability,
             unconditional repair, never fails — reports repair cost)
  audio/     miniaudio pimpl; all DSP in callback thread, game thread pokes atomics;
             procedural ambience (drones, throb, footsteps, creaks); device failure
             is non-fatal
  inference/ engine: worker thread owns llama context; queue via mutex/atomics;
             UTF-8-safe streaming, grammar constraints, graceful load failure
  ui/        ImGuiLayer lifecycle wrapper (chains GLFW callbacks with Window's)
editor/      EditorApp: own event loop, viewport = ImGui::Image of renderer FBO
             colorTexture().
             Landing screen (Screen::Landing, the default when launched with no
             project): a JetBrains-style chooser drawn full-window before any
             project opens — drawLanding() runs instead of the dockspace + scene
             render (run() early-continues; no FBO work). Left column: "LIMINAL"
             watermark (a second, larger JetBrains Mono face m_titleFont baked in
             the ctor), an extensible action list (Create new project / Quit;
             append a LandingAction to add e.g. Settings), and a bottom-left
             footer = git-branch glyph (ImDrawList) + "v<version> <commit>"
             (liminal::kVersionString / kGitCommit) that turns blue on hover and
             opens the repo (platform::openUrl) on click. Right column: recent
             projects (m_recents) as rows = hashed-color avatar + title + ~-path,
             click opens, right-click removes; empty-state hint otherwise.
             "Create new project" -> a text-path modal (parent dir + name) ->
             createProject scaffolds <dir>/<name>/{project.ljson, scenes/
             main.lscene (primary camera + cube + light via Scene::save)} then
             openProject. Recents persist via editor/recent_projects.{hpp,cpp}
             (loadRecentProjects / recordRecentProject / removeRecentProject) to
             liminal::userConfigDir()/recent_projects.json (newest-first, deduped
             by canonical path, capped 15). openProject records the project +
             flips m_screen to Editor. CLI: no args = landing, --project <p> opens
             straight into the editor, --empty = blank editor scene, --sample =
             the bundled sample project.
             File > Close Project (closeProject, enabled only with a project
             open) tears the project down — resets m_mcp/terminal/script-editor
             (recreated fresh), scene/selection, project + shader-catalog/
             m_shaderWatch state — and returns to the landing chooser. Gated
             behind a "Discard unsaved changes?" confirm modal when any script-
             editor tab is dirty (ScriptEditorPanel::anyDirty(); the editor has
             NO scene-dirty tracking, so that's the only check); clean closes
             skip the modal. TerminalPanel::stopSession() is public to support
             this teardown.
             play-in-editor = JSON snapshot before Play, fresh
             ScriptHost, restore snapshot on Stop (entity IDs reset, selection cleared).
             Camera input: handleCameraInput (RMB-fly: drag-look + WASD/QE; scroll =
             speed whenever the viewport is HOVERED or while flying — read before the
             not-flying early-return so a plain scroll over the viewport tweaks speed
             without holding RMB; a scroll arms m_camSpeedHud=1.2s → drawViewport
             paints a bottom-center "Camera speed X.X" overlay that fades its last
             0.4s, editor-only) runs in EDIT mode ONLY — early-returns in Play so scripts fully
             own the cursor (set_cursor_captured) like the standalone player; Stop
             releases any script-held capture. In Play (not paused, cursor not
             script-captured) confineCursorToViewport clamps the OS cursor to the
             Viewport window rect each frame so clicks can't leak onto other docked
             panels (Play/Pause/Stop toolbar stays reachable; pause frees the cursor).
             When a script HAS captured the cursor (GLFW_CURSOR_DISABLED), the run
             loop sets ImGuiConfigFlags_NoMouse before beginFrame so ImGui ignores
             the unbounded virtual cursor pos and won't hover/highlight panels behind
             the game (cleared when not Play-captured);
             UI font = JetBrains Mono (baked LIMINAL_EDITOR_FONT_TTF, 16px × content
             scale + inverse FontGlobalScale for retina, fs::exists fallback to default);
             script_editor (ScriptEditorPanel: tabbed text editor on ImGuiColorTextEdit
             for any text file — binary sniff via leading-NUL check + 2 MB cap,
             language highlighting by extension, plain text otherwise; dirty tabs +
             confirm-discard, Cmd/Ctrl+S save (LF-normalized), Lua-only completion +
             debounced luaL_loadbuffer compile-only diagnostics → error markers;
             reload-from-disk: a throttled (~0.5s) poll of each tab's mtime
             (stamped on open/save so our own writes never self-trigger) picks
             up external edits — clean buffers reload in place (cursor/scroll
             preserved), dirty buffers raise a "File changed on disk" conflict
             modal (Keep mine / Load disk; one at a time, m_confirmReload);
             opened by double-clicking any text file in Asset
             Browser, docked tabbed with Viewport), lua_complete (in-process completion
             engine: keywords/stdlib/
             lm.* API/buffer words, context-aware on `.`/`:`, popup anchored at text
             cursor, Ctrl+Space manual trigger — works without scripting flag),
             Build Game (Game menu → buildGame, uses buildGamePak in core/pak.cpp;
             on macOS falls back to an exe+.pak sidecar when appending a Mach-O fails
             codesigning),
             theme (editor/theme.{hpp,cpp}: self-contained ImGui theme registry —
             theme::registry() returns built-in {Dark,Light,Liminal(default),
             High Contrast} as {name, apply(ImGuiStyle&)} entries (each seeds from
             a StyleColors* base then overrides colors + layout vars), theme::find()
             by name. EditorApp::applyTheme(name) is the single switch seam (looks
             up + applies to ImGui::GetStyle(), sets m_themeName, unknown = log+no-op);
             called in the ctor after the font block to override ImGuiLayer's base
             StyleColorsDark. The "Theme" main-menu (after Game) just iterates the
             registry → applyTheme — zero menu-specific logic, so a future settings
             window reuses the same seam. m_themeName is in-memory, defaults Liminal
             each launch (persistence deferred to a settings window)),
             terminal_panel (TerminalPanel: "Terminal" dock window, tabbed in the
             center node behind Viewport. Owns a core::Pty + a libvterm VTerm; a
             reader thread drains the pty into a mutex-guarded byte queue, main-thread
             draw() feeds it to vterm_input_write and the VTerm is touched
             single-threaded by construction. Renders the cell grid via ImDrawList
             — per-cell bg rect + UTF-8 glyph, indexed/default colors resolved with
             vterm_screen_convert_color_to_rgb, 256-color + box-drawing + alt-screen +
             cursor (solid block when the panel is focused — glyph re-drawn in the bg
             color over it — hollow outline when not); mouse-wheel scrollback via
             sb_pushline/sb_popline ring.
             Mouse text-selection (left-click-drag, LINEAR reading-order span,
             translucent highlight over each cell bg; visible screen only,
             scrollback excluded; plain click clears) + clipboard copy via Cmd+C
             on macOS (selectedText() pulls the span per-row through
             vterm_screen_get_text, trailing spaces trimmed, rows joined "\n";
             Cmd+C never reaches the interrupt path so Ctrl+C stays the sole
             interrupt on every platform).
             Focused keyboard/clipboard routed through vterm_keyboard_* +
             vterm_output_read to the pty, steals keys while focused
             (SetNextFrameWantCaptureKeyboard). Lazily spawns the user's login shell
             ($SHELL, fallback `/bin/zsh -l`) on first draw. Spawn is gated until a
             working dir is set
             (setWorkingDir, called by EditorApp on project open) so the child
             starts with the opened project as its CWD; until then the panel
             shows an "Open a project to start the terminal." banner),
             mcp_server (McpServer: in-editor Model Context Protocol server so the
             `claude` in the Terminal panel can introspect live editor state.
             Transport = MCP "Streamable HTTP" via cpp-httplib — a single
             `POST /mcp` endpoint on 127.0.0.1 (plaintext, localhost only) taking
             one JSON-RPC 2.0 request and returning one JSON-RPC 2.0 response as
             application/json; no SSE (every tool is request/response).
             THREADING INVARIANT: the http server runs on its own thread(s); entt/
             Scene live on the main thread and are NOT thread-safe. Tools never
             touch scene state off-thread — each packages its scene-read into a
             std::function + std::promise on a mutex-guarded queue and blocks on
             the future (5s timeout); EditorApp calls m_mcp->pump() once per frame
             (top of run loop) to drain + run tasks ON the main thread and fulfill
             the promise. McpServer holds no Scene&; it reads via an McpProvider of
             main-thread getters EditorApp wires capturing `this`. Methods:
             initialize / tools/list / tools/call / notifications/initialized
             (no-op 202). Read-only tools: scene_tree (entities → id+Name+component
             names), get_entity (by entt id or Name → full component JSON via the
             ComponentRegistry toJson path), current_project (m_projectFile/title/
             scenePath), list_scripts (*.lua under project dir, relative), read_script
             (file contents, 512 KiB clamp, path sandboxed to project root via
             weakly_canonical prefix check), console_log (input {lines?:integer},
             default 200 → tail of m_console as {lines:[...]}), play_state (no input →
             {mode:"edit|play", paused:bool}), screenshot (no input → an MCP image
             content block {type:"image", data:<base64 PNG>, mimeType:"image/png"};
             base64 PNG of the low-res FBO via Renderer::readPixels + stb_image_write,
             row-flipped to top-down, 1 frame stale since pump() runs before the frame
             renders; returns a text error block "no framebuffer" if no FBO).
             Control tools (via McpProvider control/reloadScene/saveScene getters):
             play_game (no input → startPlay, returns {mode,paused}), pause_game
             (input {paused?:boolean} default true → pause/resume, errors if not
             playing), stop_game (no input → stopPlay), reload_scene (no input →
             openScene(m_scenePath); discards live in-memory edits and auto-stops
             Play, reloading from disk), save_scene (input {path?:string}, empty →
             current m_scenePath → m_scene.save).
             Mutation tools (via McpProvider setComponent/removeComponent/
             createEntity/destroyEntity getters, all main-thread via marshal;
             entity resolution shared with get_entity through
             EditorApp::resolveEntity(idOrName) — entt id string or Name →
             entt::null): set_component (input {id,component,data?:object} →
             ComponentRegistry::find(component)->fromJson emplace_or_replace;
             on "Transform" edits the transform, on "Script" data sets the
             entity's {"paths":[...]} list of Lua scripts — multi-script),
             remove_component (input {id,component} → ops->removeFrom),
             create_entity (input {name?:string} → m_scene.create(name); Name
             added when non-empty), destroy_entity (input {id} →
             m_scene.destroy; clears m_selected if it was the target). All
             mutations are live/in-memory — persist with save_scene (no
             auto-save). Errors: "entity not found" / "unknown component: X" /
             missing-required-arg as a text error block.
             Port: tries 7717 then up to 15 more,
             reports the bound port; 0 = none bound (non-fatal). Started on first
             project open (startMcpServer); each tool reports CURRENT m_scene (a
             Play temp scene is reported as-is). On open it also writes
             `<projectDir>/.mcp.json` (writeMcpJson): parse-and-merge so unrelated
             mcpServers survive, set the `liminal` entry to
             `{type:"http",url:"http://127.0.0.1:<port>/mcp"}` so `claude`
             auto-discovers it; write failure is logged + non-fatal).
             On project open the editor also seeds the liminal-lua Claude Code
             skill (seedLuaSkill): copies the canonical
             sample_project/.claude/skills/liminal-lua/SKILL.md (baked path
             LIMINAL_EDITOR_LUA_SKILL) into `<projectDir>/.claude/skills/
             liminal-lua/SKILL.md` if absent — never clobbers a customized one,
             skips when dest == source (the sample itself), logged + non-fatal.
             The skill documents the `lm` Lua API + script lifecycle so the
             `claude` in the Terminal panel can author scripts accurately.
             Custom shader discovery (scanShaders): on project open the editor
             scans `<projectDir>/shaders/` and registers a pack per entry, then
             rebuilds shaderCatalog() = {"native","retro", ...discovered} for the
             Camera dropdown. Layout: a SUBDIR with both scene.vert + scene.frag =
             a full pack (ShaderPack::makeFullPack, name = dir name); a lone
             `<name>.frag` = a FRAG-ONLY pack (ShaderPack::makeFragOnlyPack, name =
             file stem) — the engine wraps a default native vertex stage + the native
             frag header, so the user writes a fragment BODY ONLY (no #version /
             in / out / uniform decls). tickShaderWatch() (~0.5s mtime poll) hot-
             reloads changed shader files: re-register + recompile in place; a compile
             failure is logged to the editor console and the previous compiled pack is
             kept. Shader files open in the ScriptEditorPanel (double-click in the
             Asset Browser) with GLSL highlighting for .vert/.frag/.glsl. Both render
             paths (App::primaryCameraView, EditorApp::currentView) call
             useShaderPack(cam.shaderName) each frame (default "native" when there is
             no primary camera), so the editor previews the selected shader in edit
             mode too.
player/      liminal-player: standalone runtime. Locates its pak (appended to its own
             binary via platform::selfExePath, or a sidecar .pak), mounts the VFS
             (Assets::mountPak), runs App with hotReload off. After App is
             constructed (GL context + Renderer exist; built-ins "native"/"retro"
             always registered by the ctor) it discovers the game's CUSTOM shaders
             from the pak (registerPakShaders): enumerates pak keys via
             PakReader::paths(), mirrors the editor's on-disk layout —
             shaders/<name>/scene.vert+scene.frag = full pack, shaders/<name>.frag =
             frag-only — reads each via the VFS, builds packs with
             ShaderPack::makeFullPack / makeFragOnlyPack (the shared core helpers),
             registers them (lazy-compile) and seeds shaderCatalog() so a scene's
             Camera.shaderName resolves at runtime. No shaders/ in the pak = no-op
tests/       test_scene_roundtrip (JSON byte-stability of all components),
             test_procgen_determinism (FNV-1a hash of full pipeline vs golden),
             test_lua_procgen (Lua procgen determinism), test_pak_roundtrip,
             test_build_pak
assets/shaders/  native/ (scene.vert + scene.frag — the default crisp pack) and
             retro/ (dream.vert/.frag scene pass + blit.vert/.frag upscale, the PS1
             look). The blit pair is shared by both packs. All baked into the lib via
             src/render/embedded_shaders.hpp.in at configure time.
cmake/
```

## Conventions & invariants

- Determinism is a hard requirement in procgen: all randomness through `procgen/rng.hpp` (xorshift32, salted per stage). Never use `rand()`/`std::random_device` there; the determinism test gates this.
- `.lscene` entity IDs are debugging-only; never written code may rely on them surviving a load.
- Lua `get_component` returns raw entt pointers valid only within the frame (entt structural changes invalidate); don't cache them across frames in scripts.
- Audio: game thread must only touch atomics; DSP belongs in the miniaudio callback.
- Renderer look depends on nearest-neighbor filtering and low-res FBO — don't "fix" to linear.
- Scripting (Lua 5.4 + sol2) is always compiled in — no flag. Inference and the editor are optional via CMake flags; keep new inference/editor code gated accordingly (`#if defined(LIMINAL_WITH_INFERENCE)` blocks in `app.hpp` follow this pattern).

## Known issues / accepted limitations

All bugs and notable risks from the 2026-06-12 review were fixed on 2026-06-12 (editor picking via gizmo-freshness gate + ray-vs-AABB, Entity/Lua null guards, CMake C++ standard save/restore around llama fetch, `LIMINAL_EDITOR_SAMPLE_PROJECT` cache var, renderer ctor exception safety, stbi RAII guard, console ListClipper + 1000-line cap). Remaining accepted limitations:

- macOS shipped games are an exe + `.pak` sidecar pair (codesigning rejects a Mach-O with data appended after the load commands); Windows/Linux append the pak into a single exe.
- The editor Terminal panel is POSIX-only (macOS/Linux via forkpty). On Windows `liminal::Pty` is a stub (ConPTY not yet implemented): `spawn()` returns false and the panel shows an "unsupported on this platform" banner. Color emoji is out of scope (256-color + box-drawing only; no freetype/color-glyph wiring). The panel has no type/font-mismatch protection beyond requiring a monospace UI font (JetBrains Mono provides this) — cell metrics use `CalcTextSizeA("M")` × `GetFontSize()`.
- `runtime:` meshes registered via `lm.assets.add_mesh` don't survive a scene reload unless the script re-registers them.
- Scripts using `lm.ai` must call `forget(id)` after a request completes, or responses accumulate.
- `lm.scene.change` is unsupported during editor Play (the scene swap is deferred only in the player).
- Custom shaders: a frag-only file (`shaders/<name>.frag`) is a fragment BODY ONLY — the engine prepends `#version 410 core`, the varyings (vNormal, vViewDist, vGradT, smooth vUV) and the per-draw uniforms (uTex, uColor, uColor2, uLightDir, uAlphaTest) + supplies the native vertex stage; the file must contain just `void main(){ ... FragColor = ...; }` (no #version / in / out / uniform decls). Full packs (`shaders/<name>/scene.vert`+`scene.frag`) own both stages: attribute locations 0=pos 1=normal 2=uv, may read any per-draw uniform (uModel/uView/uProj/uNormalMat/uColor/uColor2/uGradBase/uGradInv/uTex/uLightDir + retro-only ones; unused are harmless). Discovered/custom shaders always render at Native (window) resolution.
- Editor gizmo euler composition (ImGuizmo XYZ vs Transform Y→X→Z) approximates compound rotations — documented in `drawGizmo`.
- Script editor completion popup position mirrors private upstream layout math (gutter = " maxline " + 10px mLeftMargin, line advance = bare font height, child scroll queried by recomposing BeginChild's window name via imgui_internal) — re-verify if the ImGuiColorTextEdit or ImGui pin changes. No type inference: `:` on any identifier offers Entity methods. Script-editor tab bar is intentionally not Reorderable (tab IDs are positional). Enter auto-indent is forced on for all language definitions via a mutated copy of the def in `ScriptEditorPanel::open` (upstream Lua def ships with `mAutoIndentation=false`, which disables leading-whitespace copy on newline).

Editor picking notes: clicks gate on `ImGuizmo::IsOver()` ONLY on frames where `drawGizmo` actually called `Manipulate` (returns bool) — IsOver is stale otherwise. `pickEntity` slab-tests the ray in entity-local space against the mesh's `localMin/localMax` when a MeshRenderer resolves, sphere fallback otherwise; miss = deselect.
