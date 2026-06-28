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
| `LIMINAL_HEADLESS_OFFSCREEN` | OFF | Display-less GL context for `--headless` (EGL surfaceless by default; needs libEGL — `brew install mesa` on macOS, `libegl1-mesa-dev` on Linux) |
| `LIMINAL_HEADLESS_OSMESA` | OFF | Use OSMesa (CPU) instead of EGL for the offscreen backend (needs libOSMesa) |

No install/export rules — liminal is no longer a `find_package` consumable; the lib is internal to the editor/player builds.

### Packaging (macOS)

`cmake --build build --target package-mac` assembles a relocatable `build/dist/Liminal.app` (+ `Liminal-<version>-macOS.dmg`) from the already-built editor/player binaries — APPLE-only, gated on `LIMINAL_BUILD_PLAYER`, defined in `editor/CMakeLists.txt`, logic in `cmake/pack_macos.cmake`. The in-tree targets stay PLAIN executables (no `MACOSX_BUNDLE`) so dev `./liminal-editor` and the Build-Game player-lookup (player beside the editor) are undisturbed; packaging is an explicit opt-in step. Bundle layout: `Contents/MacOS/{liminal-editor, liminal-player}` (player rides along so the bundled editor's Build Game works — it looks for the player beside itself), `Contents/Resources/{JetBrainsMono.ttf, NotoColorEmoji.ttf, Liminal.icns, skills/liminal-lua/SKILL.md}`, `Contents/Info.plist`. The app icon is built at package time by `iconutil` from the per-size PNGs in `assets/icons/macos/<variant>/liminal-{16..1024}.png` (no upscaling — every `.iconset` slot maps to a real source size) into `Liminal.icns`, referenced via `CFBundleIconFile`; the variant is the `LIMINAL_MACOS_ICON_VARIANT` cache var (`light` default, or `dark` — both full-bleed rounded-square sets live in `assets/icons/macos/`). A single classic `.icns` is one fixed icon (appearance-aware light/dark icon variants would need Asset-Catalog/Icon-Composer tooling). Both binaries link only system libs (otool-verified) so no dylib fixup is needed. The script ad-hoc codesigns (`codesign --sign -`) so it runs locally; distribution to OTHER Macs still needs a Developer ID signature + notarization (Gatekeeper rejects ad-hoc), and the macOS-shipped-games caveat (exe + `.pak` sidecar) is unrelated. **Resource relocatability:** `editor/resource_paths.hpp` `resolveResource(bundleRelName, bakedAbs)` resolves each runtime resource (fonts, skill) by preferring a copy beside the exe (`../Resources` for the `.app`, `./resources` for a portable dir) and falling back to the configure-time baked absolute path (`LIMINAL_EDITOR_FONT_TTF` etc.) for in-tree dev builds — so the same binary works both run-from-`build/` and packaged. Windows is NOT yet packaged (no cross-toolchain on macOS; needs a Windows machine or CI, and the Terminal panel's `Pty` is a Windows stub).

First-party translation units compile with `-Wall -Wextra` (the `LIMINAL_WARNING_FLAGS` var, applied target-wide to the lib/player/tests and source-scoped to the editor's own `.cpp` files so the in-tree vendored sources — ImGui, ImGuizmo, ImGuiColorTextEdit, libvterm, stb, miniaudio, glad — stay unwarned). The build also adds `-Wl,-no_warn_duplicate_libraries` when `CheckLinkerFlag` confirms support (silences the harmless duplicate-`libglfw3.a` warning from newer macOS ld).

Dependencies fetched via `cmake/Dependencies.cmake` (FetchContent): GLFW, glad, glm, entt, Dear ImGui, miniaudio, stb, nlohmann/json, sol2+Lua, llama.cpp, ImGuizmo, ImGuiColorTextEdit (BalazsJako, editor-only, compiled into the editor like ImGuizmo), libvterm (neovim mirror, editor-only; plain C core `src/*.c` + `include/vterm.h`, no usable CMake so the `.c` files compile straight into liminal-editor like ImGuizmo — include both `include/` and `src/`; pinned commit `934bc2fbf21800ac3458a499df8820ca5fb45fd3`), cpp-httplib (yhirose, editor-only, header-only; INTERFACE target `httplib::httplib` carries the include dir + Threads dep — backs the editor's MCP server; plaintext localhost only, OpenSSL/zlib/brotli forced off; pinned `v0.18.3`), JetBrains Mono (editor-only; plain TTF zip, path exported as `LIMINAL_JETBRAINS_MONO_TTF`), FreeType (editor-only; pinned `VER-2-13-3`, all optional deps disabled via `FT_DISABLE_HARFBUZZ/PNG/ZLIB/BZIP2/BROTLI` so it builds standalone — backs ImGui's `misc/freetype/imgui_freetype.cpp` color-bitmap atlas builder), Noto Color Emoji (editor-only; single CBDT/CBLC color-bitmap TTF fetched via `DOWNLOAD_NO_EXTRACT` from googlefonts/noto-emoji `v2.047`, path exported as `LIMINAL_NOTO_EMOJI_TTF`). When `LIMINAL_BUILD_EDITOR` is on, the root CMake compiles `imgui_freetype.cpp` INTO `liminal_imgui` (not just the editor exe — ImGui 1.92's core font system references the FreeType loader from imgui.cpp/imgui_draw.cpp when `IMGUI_ENABLE_FREETYPE` is set) and defines `IMGUI_ENABLE_FREETYPE` + `IMGUI_USE_WCHAR32` PUBLIC on `liminal_imgui` (WCHAR32 widens `ImWchar` to 32-bit so plane-1 emoji codepoints survive ImGui's UTF-8 decode; it changes struct layout so it must be consistent across lib/editor/player/tests). FreeType links PUBLIC into `liminal_imgui`.

## Structure

```
include/liminal/          public headers (mirrors src/), umbrella: liminal.hpp
src/
  core/      App (main loop, owns everything; ctor takes a ScriptContext, not a
             Window*), Window (GLFW+glad RAII, input), Assets (search-path resolution
             + VFS: readFile / mountPak read through a mounted pak first), AssetCache
             (name → GPU resource, "builtin:"-prefixed procedural meshes/textures,
             warn-once failures; addMesh returns the stored "runtime:" key;
             addTexture(name,Texture) likewise returns a "runtime:" key (backs
             lm.assets.add_texture). meshKeys()/textureKeys() enumerate the
             currently-loaded keys (back the MCP list_assets runtime inventory).
             builtin: mesh names include the parametric
             "builtin:form:<sides>,<twist>,<taper>[,<seed>]" (→ Mesh::form)),
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
             color FBO back to RGBA8 bottom-up — backs the MCP screenshot tool.
             Immediate-mode 2D pass (backs lm.ui): buildUiResources() (called at
             the end of buildPipeline) bakes a 128x48 font atlas from the engine-
             baked public-domain 8x8 bitmap font include/liminal/render/font8x8.hpp
             (NOT the editor JetBrains Mono), a 1x1 white texture, a "ui2d" ortho
             textured+tinted program, and a dynamic VAO/VBO. uiText/uiRect/uiLine/
             uiSize push into m_uiSolid/m_uiText batches; flushUi() is the FIRST
             statement of endFrame (FBO still bound, top-left ortho, alpha blend,
             solids then text, batches cleared) so UI lands in the scene FBO — shows
             in player blit, editor Image, MCP screenshot; in retro it lands in the
             400x300 virtual FBO),
             Shader (throws on compile/link
             fail), Mesh (interleaved pos|normal|uv, flat-shaded, no index buffer,
             procedural primitives; shares the deterministic hashU32 noise
             primitive with Texture via render/hash_detail.hpp. Origin
             convention is split: the architectural primitives (box, pyramid,
             pillar, arch, stair, plane, form) are AABB-centered via the
             anon-namespace centerMeshData(md) helper called at the end of each
             factory — local origin = visual center, so Transform.position maps
             to the middle (MCP-agent / DCC friendly); the organic props (blob,
             tree, rock, crystal) and the composite structure() keep base-on-y=0
             for terrain placement, quad is XY-centered (billboard pivot),
             groundPlane is flat at y=0. localMin/localMax derive from the final
             vertices so the gradient shader uGradBase + raycast/overlap AABBs
             stay self-consistent after the shift), Texture (procedural RGBA8 + stb_image
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
             at runtime), MeshRenderer now carries color2 (vec4 gradient TOP tint;
             color = bottom; fromJson defaults color2→color for back-compat;
             set_color also sets color2 = flat tint unless set_color2 used; both
             Lua-bound), AudioSource,
             Light (Lua-bound via bindLight: color/intensity + set_color, G4),
             Collider ({center, halfExtents}; zero halfExtents = derive from mesh
             bounds at query; Lua-bound center/half_extents; backs lm.physics),
             Billboard ({yawOnly=true}: entities with this have their model
             matrix rebuilt each frame to face the active camera — position+scale
             kept, rotation replaced; yawOnly spins about Y only, false also pitches
             toward the camera; applied in App::renderScene(const glm::vec3& camPos)
             and EditorApp::renderScene via a file-local billboardModel helper in
             each render TU — App gets the cam world pos = inverse(view)[3], the
             editor uses its m_camPos member),
             Script (holds a `paths` vector — an entity may run multiple
             scripts; legacy single-`path` JSON still loads via a fromJson fallback);
             Transform euler order yaw→pitch→roll = Y→X→Z),
             ComponentRegistry (name → toJson/fromJson/inspect/Lua-bind, singleton),
             serialize (.lscene JSON v1; entities sorted by entt id for stable output;
             unknown components warn+skip; entity IDs NOT preserved across load),
             raycast (raycastScene(scene, assets, origin, dir, maxDist) → optional
             RayHit{entity, point, normal, distance}: nearest entity whose local
             AABB the ray hits — Collider center±halfExtents when non-degenerate,
             else resolved mesh localMin/localMax; slab test in entity-local space,
             maxDist<=0 = unbounded. Shared by the editor pick (editor_app.cpp
             pickEntity, which keeps a sphere-proxy fallback for entities with no
             mesh+no collider) and lm.physics)
  script/    ScriptHost (one sol::state, one sol::environment per (entity,path)
             instance — multiple scripts per entity, m_instances keyed
             entity → vector<Instance>; on_start/on_update; pcall error dedup;
             hot reload via 0.5s mtime watch → re-init every instance sharing the
             changed path; setErrorSink routes [lua] errors and setLogSink routes
             lm.log output to a host sink — both always print to stdout/stderr too,
             the editor wires both into its console),
             FULL io + os libraries opened in the ctor (G2: sol::lib::io added, the
             old os-pruning loop deleted — scripts get complete stdlib file access;
             io reads the real filesystem, NOT the mounted pak, documented residual);
             m_modules map + importModule back lm.import),
             lua_bindings (glm vecs, Entity usertype with add_component(name,table?)/
             remove_component(name)/has_component(name) (G6: table→luaToJson→
             ComponentOps::fromJson, the MCP set_component path from Lua),
             get_component pointers frame-local
             only; the `lm` global is wired from a ScriptContext{input, audio, assets,
             renderer, inference, hotReload, requestSceneChange} that replaced the old
             Window* ctor).
             lm.* API surface:
               lm.log     — print to the engine console (host log sink)
               lm.json    — encode(value)→string / decode(string)→value (G9;
                            contiguous 1..n int keys ⇄ array else object; decode
                            raises a Lua error on malformed input)
               lm.import  — import(path)→cached module value (G5; VFS pak-first,
                            run once in the shared state, same object returned to
                            every caller = shared code+state across entity scripts;
                            cycle-safe nil placeholder; never throws)
               lm.ui      — immediate-mode screen-space 2D into the scene FBO (G1;
                            text(x,y,str,r,g,b[,a][,scale]) / rect(x,y,w,h,r,g,b[,a])
                            / quad (alias of rect) / line(x0,y0,x1,y1,r,g,b[,a][,thick])
                            / size()→(w,h); origin top-left, render-target pixels;
                            via bindUi → Renderer::uiText/uiRect/uiLine/uiSize, flushed
                            in flushUi() at the top of endFrame while the FBO is bound)
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
                            terrain_mesh / water_mesh, plus the one-shot town{seed=…}.
                            HeightField usertype also exposes height_at_world(x,z) /
                            walkable_at_world(x,z) (bilinear, mirrors
                            TerrainField::height; walkable = not underwater)
               lm.assets  — add_mesh (register a runtime: mesh) / add_texture
                            (G10; (name,path) via VFS+Texture::fromMemory OR
                            (name,w,h,pixels_table) via Texture::fromPixels →
                            AssetCache::addTexture, returns runtime: key)
               lm.render  — set_fog(r,g,b,density) / set_decay(p) / set_light(x,y,z) /
                            set_shade(x,y,z) (write into Renderer.settings, feeding the
                            same per-frame uniform sets beginFrame does) / set_uniform(
                            name, number|vec3) (arbitrary named uniform applied to the
                            active scene shader each beginFrame, after the fixed sets —
                            Shader::set no-ops names the active program doesn't declare;
                            lets custom shader packs react per-area / to a decay clock).
                            All resolve through ScriptContext.renderer; warn-once no-op
                            in a renderer-less host. (G3: lua_bindings.cpp bindRender;
                            Renderer::setFogColor/setFogDensity/setDecay/setLightDir/
                            setShadeDir/setUniform + m_customUniforms map in renderer.hpp)
               lm.physics — raycast(origin, dir [,maxDist]) → {entity, point,
                            normal, distance} or nil (over the active scene's
                            Collider/mesh AABBs via scene/raycast.cpp;
                            maxDist default 0 = unbounded) / overlap(a, b) →
                            bool (world-AABB intersection of two Entities)
  procgen/   rng (deterministic xorshift32, per-stage salted seeding), types (plain
             structs between stages), wfc (tiled-model WFC, weighted picks, restart on
             contradiction with seed+1), tileset (JSON overlay on compiled-in fallback),
             terrain/terrain_field (value noise, quantized, water/void masks;
             shared hashU32/valueNoise/smoothstep helpers in procgen/noise_detail.hpp,
             included by terrain/terrain_field/structure),
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
             Panels are CLOSABLE + MULTI-INSTANCE via a PanelInstance registry
             (m_panels): each open dock window is one {PanelKind kind, int uid,
             bool open, unique_ptr<TerminalPanel>, unique_ptr<ScriptEditorPanel>}
             — window titles carry a "##<uid>" suffix so duplicates get distinct
             ImGui ids while showing the same visible label (the part before
             "##"); Begin's p_open = &inst.open, so the window X closes just that
             instance (drawUi erases open==false entries after the draw loop;
             Terminal kind calls stopSession() first). seedDefaultPanels() seeds
             one-of-each at FIXED uids (Hierarchy=1, Inspector=2, Viewport=3,
             AssetBrowser=4, Console=5, Terminal=6, ScriptEditor=7) so
             buildDefaultLayout docks by the "Title##uid" strings; addPanel(kind)
             appends at the next uid (Terminal/ScriptEditor construct their
             sub-panel via makeTerminal/makeScriptEditor, wired identically to the
             defaults). LAYOUT PERSISTENCE: the ctor points io.IniFilename at a
             stable per-user file userConfigDir()/editor_layout.ini (held in the
             m_iniPath member — ImGui keeps the raw const char*) instead of the
             CWD-relative default, so the dock layout reliably round-trips across
             launches (and run-from-build vs packaged .app). buildDefaultLayout
             still only fires on the first frame with no saved node (line-759
             guard), so first launch builds the base layout and ImGui auto-saves
             it. RESET LAYOUT: Tools menu → "Reset Layout" (after the New-panel
             items) calls resetLayout() which sets m_resetLayout (deferred — the
             menu runs after the dockspace block); the next drawUi top
             DockBuilderRemoveNode(dockId) + seedDefaultPanels() (re-seeds the 7
             defaults at uids 1..7, reopening any closed/minimized panels, stops
             live terminals) + buildDefaultLayout, before DockSpace + the panel
             loop (iteration-safe). A new "Tools" main-menu (after Theme) spawns a
             fresh instance of any panel (New Hierarchy / Inspector / Viewport /
             Asset Browser / Console / Terminal / ScriptEditor) — it runs in drawMenuBar
             BEFORE the panel draw loop, so the addPanel append is iteration-safe
             (the Asset-Browser "open file when no ScriptEditor exists" path
             instead DEFERS its spawn to after the loop via
             m_pendingOpenInNewScriptEditor, since spawning mid-loop would
             reallocate m_panels). SHARED vs per-instance: m_selected / m_console
             / m_assetTree stay shared, so duplicate Hierarchy/Inspector/Console/
             Asset-Browser windows mirror the same underlying data; Terminal +
             ScriptEditor own their state per-instance (two Terminals = two
             independent shells). Singleton-replacing helpers: anyScriptEditorDirty
             (Close-Project guard), anyScriptEditorFocused / anyTerminalFocused
             (undo-chord suppression OR'd across instances), scriptEditorForOpen
             (focused else first ScriptEditor for the Asset-Browser open route).
             VIEWPORT INPUT: only ONE viewport drives camera/pick/cursor-confine —
             m_activeViewportUid (resolved at the top of drawUi; falls back to the
             first Viewport if its owner was closed). drawViewport runs
             handleCameraInput/gizmo/pickEntity + records the confine rect only
             when inst.uid == m_activeViewportUid; focusing a viewport claims
             ownership for the NEXT frame (set AFTER the isActive read, so grabbing
             focus never double-handles input the same frame). All viewports share
             the single renderer/FBO, so non-active ones are passive mirrors of the
             same camera view (accepted limitation). ScriptEditorPanel::draw /
             TerminalPanel::draw take a (windowTitle, p_open) so each instance
             Begins its own "##<uid>" window. closeProject tears down ALL instances
             and re-seeds the default set (seedDefaultPanels, terminals stopped
             first), resetting uids.
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
             straight into the editor, --empty = blank editor scene,
             --headless --project <p> = GUI-less mode (below).
             HEADLESS MODE (--headless, editor/main.cpp): runs the SAME EditorApp
             with a HIDDEN GL window (Window ctor gained a `visible` param =
             GLFW_VISIBLE off + vsync off — the context + offscreen FBO still work
             so Renderer/readPixels/screenshot are fully functional) and NO ImGui
             drawing. The ctor takes `headless` + `offscreen` bools; when headless
             is set it opens the project via openOrCreateProject (no landing
             fallback — the chooser is GUI-only) and main() calls runHeadless()
             instead of run().
             runHeadless() is an engine-only mirror of run(): m_mcp->pump() +
             tickShaderWatch() + (in Play) m_scripts->update + the deferred
             lm.scene.change swap + scene render into the FBO, NO
             beginFrame/drawUi/endFrame/swapBuffers, ~30fps sleep cap (no vsync).
             The MCP server (startMcpServer, ALL ~30 tools incl. screenshot via
             the offscreen FBO, .mcp.json + liminal-lua skill seeding) is the sole
             driver — an external `claude`/MCP client pointed at the project dir
             runs it; nothing editor-specific is stubbed (selection/console/camera
             state all exist and work). log() echoes to stdout when m_headless so
             the bound MCP URL + project/script logs are visible. --headless
             REQUIRES --project (enforced in main → exit 2); an empty/new dir is
             auto-scaffolded by scaffoldProject(root,title) (extracted from
             createProject; writes project.ljson + scenes/main.lscene into the dir
             itself, titled after its folder). Exit via SIGINT/SIGTERM →
             requestQuit() (sets the close flag).
             DISPLAY-LESS (offscreen) backend (LIMINAL_HEADLESS_OFFSCREEN, OFF by
             default): with the hidden GLFW window, --headless still needs a
             DISPLAY SERVER to create the GL context (WindowServer on macOS,
             X11/Wayland on Linux) — useless on a bare-SSH/CI box. Turn the option
             ON and headless instead creates a TRULY display-less context via
             OffscreenContext (include/liminal/core/offscreen_context.{hpp,cpp},
             compiled into the liminal lib only when a backend is enabled, gated by
             the LIMINAL_HAS_OFFSCREEN umbrella macro the header sets):
               • EGL (default) — eglGetPlatformDisplay surfaceless / EXT_platform_
                 device, eglBindAPI(EGL_OPENGL_API), no-config + EGL_NO_SURFACE
                 context at LIMINAL_GL_MAJOR/MINOR core. Portable: a real GPU when
                 present (NVIDIA / Mesa HW, full GL 4.6), llvmpipe SOFTWARE
                 otherwise. Works on Linux AND macOS via Homebrew Mesa's libEGL
                 (`brew install mesa`; note current Homebrew Mesa ships libEGL but
                 NOT libOSMesa). CMake define LIMINAL_HEADLESS_EGL.
               • OSMesa (opt-in, -DLIMINAL_HEADLESS_OSMESA=ON) — pure-CPU
                 OSMesaCreateContextAttribs into a held client buffer; for boxes
                 with libOSMesa but no usable EGL. CMake define LIMINAL_HEADLESS_OSMESA.
             gladLoadGL reuses the EXISTING desktop-GL loader via the backend's
             eglGetProcAddress/OSMesaGetProcAddress — no glad regeneration. The
             engine renders into its own FBO and readPixels reads THAT back, so
             screenshots + ALL ~30 MCP tools are byte-for-byte unaffected (proven:
             screenshot of a builtin:pyramid scene renders correctly through
             llvmpipe with no window). When offscreen, Window holds an
             OffscreenContext instead of a GLFWwindow (m_window stays null) and
             every GLFW-touching method short-circuits: shouldClose/requestClose →
             m_closeFlag, framebufferSize → the stored size, time() →
             steady_clock, pollEvents/swapBuffers/setVsync + all input → no-op/zero,
             handle() → nullptr. ImGuiLayer keys off handle()==nullptr: it always
             creates the ImGui context (the editor ctor touches GetIO()/fonts even
             headless) but SKIPS the GLFW + OpenGL3 BACKENDS (m_active=false), so
             beginFrame/endFrame no-op — runHeadless never draws UI anyway. If the
             option is OFF (or the backend fails at runtime) Window falls back to
             the hidden GLFW window, so --headless still works wherever a display
             server exists. SELECTION: main.cpp `--display auto|offscreen|glfw`
             (default auto = offscreen iff a backend was compiled in, else hidden
             window; offscreen forces it, warns + falls back if none compiled; glfw
             forces the hidden window for local A/B on a Mac with a display). The
             [gl] startup line tags the active context, e.g.
             "llvmpipe ... (offscreen: EGL)" vs "Apple M1 Pro". Caveat: software
             llvmpipe is correct but slow — fine for screenshots, not high-fps
             interactive; a GPU instance with the EGL device platform gets HW
             acceleration. The CMake find probes /opt/homebrew/opt/mesa +
             /usr/local/opt/mesa for libEGL/libOSMesa (Homebrew keg) plus system
             paths; the Homebrew libEGL's absolute install_name means no DYLD_*
             env is needed at runtime.
             File > Close Project (closeProject, enabled only with a project
             open) tears the project down — resets m_mcp, re-seeds the default
             panel set (seedDefaultPanels: all instances dropped, terminals
             stopped first, fresh one-of-each), scene/selection, project +
             shader-catalog/m_shaderWatch state — and returns to the landing
             chooser. Gated behind a "Discard unsaved changes?" confirm modal when
             ANY script-editor instance is dirty (anyScriptEditorDirty() over
             ScriptEditorPanel::anyDirty(); the editor has NO scene-dirty
             tracking, so that's the only check); clean closes skip the modal.
             TerminalPanel::stopSession() is public to support this teardown.
             play-in-editor = JSON snapshot before Play, fresh
             ScriptHost (built via buildPlayScriptHost, shared by startPlay and
             the deferred scene-change swap), restore snapshot on Stop (entity IDs
             reset, selection cleared). A mid-Play lm.scene.change is honored: the
             requestSceneChange lambda stashes the path in m_pendingPlayScene and
             the run loop swaps the scene (Assets::resolve + Scene::load) +
             rebuilds the ScriptHost right after the script update — WITHOUT
             touching m_playSnapshot, so Stop still returns to the original
             pre-Play scene (the swapped-to scene is discarded).
             undo/redo (edit_history.{hpp,cpp}: EditHistory — whole-scene JSON
             snapshot stacks reusing sceneToJson/sceneFromJson, bounded ~100
             deep. Edit mode ONLY. Two entry points: pushSnapshot(scene) called
             BEFORE discrete mutations (hierarchy create/duplicate/delete,
             inspector add/remove component, all four MCP mutation tools), and
             tick(scene, ImGui::IsAnyItemActive()||ImGuizmo::IsUsing()) once at
             the end of drawUi() to coalesce a whole gizmo/DragFloat3 drag into a
             single undo entry (commits the pre-drag "clean" baseline on the
             interaction's release edge if the scene changed). EditorApp::doUndo/
             doRedo remember the selected entity's Name, restore, then
             selectByName() to re-resolve selection (entt-ids don't survive the
             load). Cleared on scene/project open, newScene, closeProject, and
             Play start/stop. Wired to: Edit menu (Undo Cmd+Z / Redo Cmd+Y),
             viewport-toolbar Undo/Redo buttons (BeginDisabled on empty stacks),
             and the global chord next to Cmd+S — Cmd/Ctrl+Z undo, Cmd/Ctrl+Y or
             Cmd/Ctrl+Shift+Z redo, suppressed while the Script Editor or Terminal
             is focused (TerminalPanel::focused() added) so those panels keep
             their own undo: the ImGuiColorTextEdit widget already has a full
             native undo/redo (Cmd+Z/Cmd+Y on macOS via io.ConfigMacOSXBehaviors)
             and the terminal's Ctrl+Z goes to the shell — neither is reimplemented).
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
             Noto Color Emoji (baked LIMINAL_NOTO_EMOJI_TTF) is MERGE-mode merged over
             the body face at the same size with ImFontConfig.FontLoaderFlags |=
             ImGuiFreeTypeBuilderFlags_LoadColor (the 1.92 rename of FontBuilderFlags)
             so color emoji + symbol glyphs render (used by the Terminal panel); the
             FreeType atlas builder (IMGUI_ENABLE_FREETYPE, compiled into liminal_imgui)
             is mandatory for color glyphs, and IMGUI_USE_WCHAR32 is required so plane-1
             emoji codepoints aren't collapsed to U+FFFD. The merge is guarded by
             fs::exists(LIMINAL_NOTO_EMOJI_TTF) — a missing emoji TTF logs + skips, the
             editor still runs (#include <imgui_freetype.h> for the LoadColor flag);
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
             StyleColorsDark. The "Theme" main-menu (Game → Theme → Tools order)
             just iterates the
             registry → applyTheme — zero menu-specific logic, so a future settings
             window reuses the same seam. m_themeName is in-memory, defaults Liminal
             each launch (persistence deferred to a settings window)),
             terminal_panel (TerminalPanel: "Terminal" dock window, tabbed in the
             center node behind Viewport. Owns a core::Pty + a libvterm VTerm; a
             reader thread drains the pty into a mutex-guarded byte queue, main-thread
             draw() feeds it to vterm_input_write and the VTerm is touched
             single-threaded by construction. Renders the cell grid via ImDrawList
             — per-cell bg rect + UTF-8 glyph, indexed colors resolved with
             vterm_screen_convert_color_to_rgb; the terminal-DEFAULT fg/bg
             (VTERM_COLOR_IS_DEFAULT_FG/BG) now derive from the live ImGui theme via
             ImGui::GetColorU32(ImGuiCol_Text)/(ImGuiCol_WindowBg) in colorToImU32 so
             the panel follows applyTheme (those return IM_COL32-packed ImU32 — same
             0xAABBGGRR layout as packRGB, no channel swap; older scrollback is
             captured-at-the-time and won't re-tint on a theme switch). 256-color +
             box-drawing + color emoji (via the FreeType/Noto merge above) + alt-screen +
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
             base64 PNG of the scene FBO (window resolution for the default native
             pack, the virtual low-res FBO for retro) via Renderer::readPixels +
             stb_image_write, row-flipped to top-down, 1 frame stale since pump()
             runs before the frame renders; returns a text error block
             "no framebuffer" if no FBO).
             Discovery/introspection tools (no scene mutation): list_components
             (no input → {components:[{name,fields:{<f>:{type,default}}}]};
             built by default-constructing each ComponentRegistry::all() entry on
             a scratch entt::registry via fromJson({}) then reading toJson, with a
             type tag inferred from the default JSON value shape — so set_component
             payloads use the exact serialized field names, not guesses),
             list_assets (no input → {builtin_meshes, builtin_textures (the
             asset_cache.hpp catalog), project_textures (png/jpg/jpeg/tga/bmp walk
             of the project dir), runtime_meshes/runtime_textures (AssetCache::
             meshKeys()/textureKeys() filtered to runtime: keys), shader_packs
             (shaderCatalog())}), list_scenes (no input → *.lscene under the
             project dir, relative — mirrors list_scripts), list_shaders (no input
             → {packs: shaderCatalog(), cameras:[{id,name,shader,primary}]}).
             Control tools (via McpProvider control/reloadScene/saveScene/openScene/
             newScene getters): play_game (no input → startPlay, returns
             {mode,paused}), pause_game (input {paused?:boolean} default true →
             pause/resume, errors if not playing), stop_game (no input → stopPlay),
             reload_scene (no input → openScene(m_scenePath); discards live
             in-memory edits and auto-stops Play, reloading from disk), save_scene
             (input {path?:string}, empty → current m_scenePath → m_scene.save),
             open_scene (input {path} → openScene(path); resolves via Assets,
             auto-stops Play), new_scene (no input → newScene()), build_game (input
             {path?:string}, empty → <projectDir>/build/<title> → buildGame();
             synchronous on the main thread, a large build may exceed the 5s
             marshal window so the caller gets a timeout while the build still
             completes — poll console_log). Feedback tools: select_entity (input
             {id} → set m_selected, returns {ok,id,name}), get_selection (no input →
             {id,name} or null), focus_entity (input {id} → point the editor camera
             at the entity's Transform: m_camPos = pos + a back-off diagonal, then
             m_camYaw/m_camPitch via atan2/asin of the look vector — pairs with
             screenshot; edit mode). Query tool: raycast (input {origin:[x,y,z],
             dir:[x,y,z], maxDist?} → wraps raycastScene(m_scene, m_assets, ...) →
             {entity,name,point,normal,distance} or null). validate_scene (no input
             → {ok, issues:[{entity,name,severity,message}], primaryCameraCount};
             checks every MeshRenderer.meshAsset/textureAsset resolves via
             m_assets.mesh()/texture(), every Script.paths[] file exists under the
             project dir, and the primary-Camera count is exactly one).
             Mutation tools (via McpProvider setComponent/removeComponent/
             createEntity/destroyEntity/duplicateEntity getters, all main-thread
             via marshal; entity resolution shared with get_entity through
             EditorApp::resolveEntity(idOrName) — entt id string or Name →
             entt::null; all push an undo snapshot first): set_component (input
             {id,component,data?:object} → ComponentRegistry::find(component)->
             fromJson emplace_or_replace; on "Transform" edits the transform, on
             "Script" data sets the entity's {"paths":[...]} list of Lua scripts —
             multi-script), remove_component (input {id,component} → ops->removeFrom),
             create_entity (input {name?:string} → m_scene.create(name); Name
             added when non-empty), destroy_entity (input {id} →
             m_scene.destroy; clears m_selected if it was the target),
             duplicate_entity (input {id} → duplicateEntity(src); copies all
             components, returns {ok,id,name}). All mutations are live/in-memory —
             persist with save_scene (no auto-save). Errors: "entity not found" /
             "unknown component: X" / missing-required-arg as a text error block.
             ~30 tools total (mcp_server.cpp toolSchemas() + callTool() if-chain,
             one McpProvider getter each wired in startMcpServer()).
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
             editor/skills/liminal-lua/SKILL.md (baked path
             LIMINAL_EDITOR_LUA_SKILL) into `<projectDir>/.claude/skills/
             liminal-lua/SKILL.md` if absent — never clobbers a customized one,
             skips when dest == source, logged + non-fatal.
             The skill is agent-first: it leads with a "Driving liminal as an
             agent (MCP)" section (the division of labor — MCP = live editor state,
             the agent's own file tools = .lua/.lscene on disk; the build loop;
             a full ~30-tool MCP catalog table; live-vs-persisted) and a
             Conventions section, then documents the `lm` Lua API + script
             lifecycle AND the `.lscene` scene-file JSON schema (top-level
             `"liminal_scene": 1` version key, entities/components layout, exact
             per-component field names) so the `claude` in the Terminal panel can
             drive the editor over MCP, author scripts, AND hand-write/repair scene
             files accurately. Has a table of contents.
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
             update_check (editor/update_check.{hpp,cpp}, namespace liminal::editor):
             self-update warning. On the FIRST project open EditorApp::startUpdateCheck()
             (guarded by m_updateCheckStarted = once per editor session, called at the
             end of openProject after startMcpServer) spawns a DETACHED std::thread that
             fetchLatestReleaseTag()s GitHub's
             api.github.com/repos/wilcus-industries/liminal/releases/latest (latest
             STABLE = non-prerelease/non-draft). There is NO HTTPS client in the tree
             (cpp-httplib is plaintext-only, no curl/libcurl linked), so the fetch
             SHELLS OUT to system `curl` (the platform::openUrl std::system idiom) into
             a temp file (temp_directory_path()/liminal_update_check.json), parsed with
             nlohmann (tag_name). The worker owns a copy of a shared_ptr<UpdateCheckState>
             {atomic Status Checking|UpToDate|OutOfDate|Failed + mutex'd latestTag} so it
             stays safe past EditorApp destruction (no join). parseSemver (strips leading
             v, tolerates -rc/+build suffix) + isNewer (strict semver >) compare the tag
             to kVersionString; behind → OutOfDate. drawMenuBar() calls drawUpdateWarning()
             (after the scene-path TextDisabled, before EndMenuBar) which RIGHT-ALIGNS a
             red (IM_COL32(230,60,60)) clickable "⚠ Update available: <tag>" — hover =
             hand cursor + tooltip, click = openUrl(kRepoUrl + "/releases/latest"). Any
             failure (no curl, offline, no published release → 404, parse error) resolves
             to "" → Failed → NO warning (convenience state, never load-bearing). Requires
             system curl (macOS/most Linux/Win10+); unauth GitHub API = 60 req/hr, the
             once-per-session guard keeps us far under it.
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
             test_lua_procgen (Lua procgen determinism), test_lua_api
             (headless lm.json round-trip + lm.import shared-state cache +
             raycastScene Collider-AABB hit/miss + mass create/destroy via a
             Script — the lm.scene.each collect-then-destroy UAF regression),
             test_pak_roundtrip,
             test_build_pak (buildGamePak end-to-end over a temp project the
             test synthesizes on disk — no checked-in fixture)
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
- The retro pack depends on nearest-neighbor texture filtering and its low-res FBO — don't "fix" either to linear/high-res. (The default native pack renders into a window-resolution FBO; nearest texture filtering still applies engine-wide.)
- Scripting (Lua 5.4 + sol2) is always compiled in — no flag. Inference and the editor are optional via CMake flags; keep new inference/editor code gated accordingly (`#if defined(LIMINAL_WITH_INFERENCE)` blocks in `app.hpp` follow this pattern).

## Known issues / accepted limitations

All bugs and notable risks from the 2026-06-12 review were fixed on 2026-06-12 (editor picking via gizmo-freshness gate + ray-vs-AABB, Entity/Lua null guards, CMake C++ standard save/restore around llama fetch, renderer ctor exception safety, stbi RAII guard, console ListClipper + 1000-line cap).

2026-06-15: fixed an editor SIGSEGV (UAF) when a Lua script stored entities from `lm.scene.each` / `lm.scene.find_all` and used them after the callback (the "collect-then-destroy" pattern, e.g. a `clear_world()` mass-destroy). Both binders passed/stored the C++ `Entity` **lvalue**, so sol2 handed Lua a *reference* to a reused stack local; after the call the slot held garbage, and `entity:destroy()` dereferenced a non-null garbage `Scene*` in `Entity::valid()`. Fix: push Lua-OWNED copies — `each` calls `fn(Entity(h, sc))` (rvalue), `find_all` stores `sol::make_object(sv, e)` (`src/script/lua_bindings.cpp`). Invariant: when handing a C++ value-handle (`Entity`) to Lua, always pass an rvalue or `make_object` so Lua owns the copy; never the lvalue (which binds by reference). Covered by `tests/test_lua_api.cpp` `testMassDestroy` (build N entities → `each`-collect → `:destroy()` → rebuild; asserts none survive). Note the *separate*, still-current hazard below: raw component pointers from `get_*`/`get_component` stay frame-local. Remaining accepted limitations:

- macOS shipped games are an exe + `.pak` sidecar pair (codesigning rejects a Mach-O with data appended after the load commands); Windows/Linux append the pak into a single exe.
- The editor Terminal panel is POSIX-only (macOS/Linux via forkpty). On Windows `liminal::Pty` is a stub (ConPTY not yet implemented): `spawn()` returns false and the panel shows an "unsupported on this platform" banner. Color emoji is now supported (ImGui's FreeType atlas builder + Noto Color Emoji merged over JetBrains Mono with the LoadColor flag, IMGUI_USE_WCHAR32 for plane-1 codepoints; gracefully skipped if the emoji TTF isn't present). The panel has no type/font-mismatch protection beyond requiring a monospace UI font (JetBrains Mono provides this) — cell metrics use `CalcTextSizeA("M")` × `GetFontSize()`.
- `runtime:` meshes/textures registered via `lm.assets.add_mesh`/`add_texture` DO survive a scene reload, play/stop, and `lm.scene.change`: the `AssetCache` is a value member of `App`/`EditorApp` that is never reconstructed on those paths (only the `Scene` + `ScriptHost` are rebuilt), so the keys persist for the cache's lifetime. They are lost only when the cache itself is destroyed (editor Close Project, or process exit). Scripts may still re-register idempotently in `on_start` (a re-add overwrites), but it isn't required for the key to resolve after a reload. Corollary (now documented in the liminal-lua `SKILL.md` for the editor `claude` agent): textures get NO hot-reload — a texture file is decoded once into a GPU resource and cached for the cache's lifetime (only shaders are mtime-watched, via `tickShaderWatch`); editing a texture file on disk does not refresh the screen, and none of scene reload / play-stop / `lm.scene.change` / the `reload_scene` MCP tool rebuild the cache. Re-pick it up by re-calling `lm.assets.add_texture(name, path)` with the same name, using a fresh name, or closing + reopening the project.
- Scripts using `lm.ai` must call `forget(id)` after a request completes, or responses accumulate.
- `lm.scene.change` IS supported during editor Play: like the player, the swap is deferred to just after the per-frame script update (`run()` loop, right after `m_scripts->update`) — the requested path is stashed in `m_pendingPlayScene` by the Play `requestSceneChange` lambda, then resolved via `Assets::resolve` + `Scene::load`, selection cleared, and the ScriptHost rebuilt (`buildPlayScriptHost`) against the new scene. The pre-Play snapshot (`m_playSnapshot`) is left untouched, so pressing Stop still restores the ORIGINAL pre-Play scene and the swapped-to scene is discarded (`stopPlay` also clears `m_pendingPlayScene` to drop a swap requested the same frame as Stop).
- Editor undo/redo restores whole-scene JSON snapshots (entt-ids reset, same as the Play restore); selection is re-resolved by `Name` after each step, so an undone/redone entity with no unique Name may lose selection. Undo/redo is Edit-mode only and capped at ~100 steps. The Script Editor (TextEditor's own UndoBuffer) and Terminal (shell) keep their own undo while focused; the global scene chord is suppressed for them.
- Editor panels are multi-instance (Tools menu → New <panel>): duplicate Hierarchy/Inspector/Console/Asset-Browser windows all mirror the SAME shared state (m_selected / m_console / m_assetTree) — they are views, not independent documents. Multiple Viewports all share the single renderer/FBO, so they show the SAME camera view; only one (m_activeViewportUid, the last-focused) drives camera fly / gizmo / picking / cursor-confine each frame, the rest are passive mirrors. Closing all instances of a kind is allowed; closeProject re-seeds the default one-of-each. Two ScriptEditor instances showing their internal "Discard changes?" modals are keyed per-window-id so they don't collide, but visually overlap.
- Custom shaders: a frag-only file (`shaders/<name>.frag`) is a fragment BODY ONLY — the engine prepends `#version 410 core`, the varyings (vNormal, vViewDist, vGradT, smooth vUV) and the per-draw uniforms (uTex, uColor, uColor2, uLightDir, uAlphaTest) + supplies the native vertex stage; the file must contain just `void main(){ ... FragColor = ...; }` (no #version / in / out / uniform decls). Full packs (`shaders/<name>/scene.vert`+`scene.frag`) own both stages: attribute locations 0=pos 1=normal 2=uv, may read any per-draw uniform (uModel/uView/uProj/uNormalMat/uColor/uColor2/uGradBase/uGradInv/uTex/uLightDir + retro-only ones; unused are harmless). Discovered/custom shaders always render at Native (window) resolution.
- `lm.ui` is single-material flat 2D (engine-baked 8x8 bitmap font, no kerning/Unicode beyond printable ASCII); in the retro pack it lands in the 400x300 virtual FBO and upscales with the scene. Per-vertex MeshRenderer color is a top/bottom gradient (color2) only — per-submesh material slots are out of scope (G14): a mesh draws with one color/texture pair.
- Script editor completion popup position mirrors private upstream layout math (gutter = " maxline " + 10px mLeftMargin, line advance = bare font height, child scroll queried by recomposing BeginChild's window name via imgui_internal) — re-verify if the ImGuiColorTextEdit or ImGui pin changes. No type inference: `:` on any identifier offers Entity methods. Script-editor tab bar is intentionally not Reorderable (tab IDs are positional). Enter auto-indent is forced on for all language definitions via a mutated copy of the def in `ScriptEditorPanel::open` (upstream Lua def ships with `mAutoIndentation=false`, which disables leading-whitespace copy on newline).

Editor picking notes: clicks gate on `ImGuizmo::IsOver()` ONLY on frames where `drawGizmo` actually called `Manipulate` (returns bool) — IsOver is stale otherwise. `pickEntity` slab-tests the ray in entity-local space against the mesh's `localMin/localMax` when a MeshRenderer resolves, sphere fallback otherwise; miss = deselect.
