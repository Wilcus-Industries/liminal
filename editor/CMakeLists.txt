# liminal-editor: dockable ImGui scene editor. ImGuizmo is compiled straight
# into the executable (no upstream build system); its sources come from the
# FetchContent populate in cmake/Dependencies.cmake.
# libvterm core: pure C, compiled straight in (it has no usable CMake). The
# .c files include both <vterm.h> (include/) and internal headers/.inc files
# from src/, so both dirs go on the include path below.
set(LIBVTERM_SOURCES
    ${libvterm_SOURCE_DIR}/src/vterm.c
    ${libvterm_SOURCE_DIR}/src/screen.c
    ${libvterm_SOURCE_DIR}/src/state.c
    ${libvterm_SOURCE_DIR}/src/pen.c
    ${libvterm_SOURCE_DIR}/src/parser.c
    ${libvterm_SOURCE_DIR}/src/keyboard.c
    ${libvterm_SOURCE_DIR}/src/mouse.c
    ${libvterm_SOURCE_DIR}/src/encoding.c
    ${libvterm_SOURCE_DIR}/src/unicode.c)

add_executable(liminal-editor
    main.cpp
    editor_app.cpp
    recent_projects.cpp
    edit_history.cpp
    theme.cpp
    script_editor.cpp
    lua_complete.cpp
    terminal_panel.cpp
    mcp_server.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
    ${imguicolortextedit_SOURCE_DIR}/TextEditor.cpp
    ${LIBVTERM_SOURCES})

target_include_directories(liminal-editor PRIVATE
    ${imguizmo_SOURCE_DIR}/src
    ${imguicolortextedit_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/misc/freetype  # imgui_freetype.h for the LoadColor flag
    ${libvterm_SOURCE_DIR}/include
    ${libvterm_SOURCE_DIR}/src)

# The FreeType atlas builder (imgui_freetype.cpp) is compiled into liminal_imgui
# in the root CMakeLists when LIMINAL_BUILD_EDITOR is on; IMGUI_ENABLE_FREETYPE
# and the freetype link reach this target PUBLIC-ly through liminal::imgui. The
# editor only needs the misc/freetype include dir above so editor_app.cpp can
# include <imgui_freetype.h> for ImGuiFreeTypeBuilderFlags_LoadColor.

# Warn on our own editor sources only — the in-tree vendored sources (ImGuizmo,
# ImGuiColorTextEdit, libvterm) share this target but must not be warned on.
set_source_files_properties(
    main.cpp editor_app.cpp recent_projects.cpp theme.cpp
    script_editor.cpp lua_complete.cpp terminal_panel.cpp mcp_server.cpp
    PROPERTIES COMPILE_OPTIONS "${LIMINAL_WARNING_FLAGS}")

# glfw for GLFW_KEY_* constants; liminal_imgui already carries the ImGui
# include dirs (incl. imgui_internal.h for the DockBuilder API).
target_link_libraries(liminal-editor PRIVATE liminal::liminal liminal::imgui glfw)

# cpp-httplib (header-only) for the in-editor MCP server. Its INTERFACE target
# carries the include dir and a Threads::Threads dependency (the HTTP server
# needs pthreads).
target_link_libraries(liminal-editor PRIVATE httplib::httplib)

# stb_image_write.h backs the MCP screenshot tool's PNG encoder. liminal links
# liminal_stb PRIVATE, so its include dir doesn't reach us transitively — name
# it explicitly to put the stb headers on the editor include path.
target_link_libraries(liminal-editor PRIVATE liminal_stb)

# Diagnostics compile the buffer with luaL_loadstring in a throwaway lua_State,
# so the pane needs the Lua C API directly. liminal already links lua_static
# PUBLIC, so the headers/lib reach us transitively — but name it explicitly to
# match how src/ links Lua and to stay correct if that interface ever narrows.
if(TARGET lua_static)
    target_link_libraries(liminal-editor PRIVATE lua_static)
endif()

# Bakes the sample project location in so `./liminal-editor` with no args
# opens something immediately. Overridable; only baked when the file actually
# exists at configure time (stale absolute paths fall back to empty, and the
# editor's runtime handling of an empty default path applies).
set(LIMINAL_EDITOR_SAMPLE_PROJECT
    "${CMAKE_CURRENT_SOURCE_DIR}/sample_project/project.ljson"
    CACHE FILEPATH "Default project the editor opens when run with no args")
if(EXISTS "${LIMINAL_EDITOR_SAMPLE_PROJECT}")
    target_compile_definitions(liminal-editor PRIVATE
        LIMINAL_EDITOR_SAMPLE_PROJECT="${LIMINAL_EDITOR_SAMPLE_PROJECT}")
else()
    target_compile_definitions(liminal-editor PRIVATE
        LIMINAL_EDITOR_SAMPLE_PROJECT="")
endif()

# Bakes the path to the canonical liminal-lua Claude Code skill (lives in the
# sample project). On project open the editor seeds this into the opened
# project's .claude/skills/ if absent, so Claude Code running in the editor's
# terminal knows the `lm` Lua API. Empty when missing -> seeding is skipped.
set(LIMINAL_EDITOR_LUA_SKILL
    "${CMAKE_CURRENT_SOURCE_DIR}/sample_project/.claude/skills/liminal-lua/SKILL.md")
if(EXISTS "${LIMINAL_EDITOR_LUA_SKILL}")
    target_compile_definitions(liminal-editor PRIVATE
        LIMINAL_EDITOR_LUA_SKILL="${LIMINAL_EDITOR_LUA_SKILL}")
else()
    target_compile_definitions(liminal-editor PRIVATE
        LIMINAL_EDITOR_LUA_SKILL="")
endif()

# Bakes the JetBrains Mono path fetched in cmake/Dependencies.cmake; the editor
# loads it as its default ImGui font (existence is re-checked at runtime).
if(LIMINAL_JETBRAINS_MONO_TTF)
    target_compile_definitions(liminal-editor PRIVATE
        LIMINAL_EDITOR_FONT_TTF="${LIMINAL_JETBRAINS_MONO_TTF}")
endif()

# Bakes the Noto Color Emoji path fetched in cmake/Dependencies.cmake; merged
# over JetBrains Mono into the editor atlas with the FreeType LoadColor flag.
# Runtime fs::exists guards a missing download, so this is best-effort.
if(LIMINAL_NOTO_EMOJI_TTF)
    target_compile_definitions(liminal-editor PRIVATE
        LIMINAL_NOTO_EMOJI_TTF="${LIMINAL_NOTO_EMOJI_TTF}")
endif()

# macOS packaging: assemble a relocatable Liminal.app (+ .dmg) from the built
# editor/player binaries and the bundled resources, via cmake/pack_macos.cmake.
# The in-tree targets stay plain executables (so dev `./liminal-editor` and the
# Build-Game player lookup are undisturbed); packaging is an explicit, opt-in
# `cmake --build build --target package-mac`.
if(APPLE AND LIMINAL_BUILD_PLAYER)
    add_custom_target(package-mac
        COMMAND ${CMAKE_COMMAND}
            -DEDITOR_EXE=$<TARGET_FILE:liminal-editor>
            -DPLAYER_EXE=$<TARGET_FILE:liminal-player>
            -DFONT_TTF=${LIMINAL_JETBRAINS_MONO_TTF}
            -DEMOJI_TTF=${LIMINAL_NOTO_EMOJI_TTF}
            -DSKILL_MD=${LIMINAL_EDITOR_LUA_SKILL}
            -DSAMPLE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/sample_project
            -DOUT_DIR=${CMAKE_BINARY_DIR}/dist
            -DVERSION=${PROJECT_VERSION}
            -P ${CMAKE_SOURCE_DIR}/cmake/pack_macos.cmake
        DEPENDS liminal-editor liminal-player
        VERBATIM
        COMMENT "Packaging Liminal.app + .dmg into build/dist")
endif()
