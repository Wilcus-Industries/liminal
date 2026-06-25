cmake_minimum_required(VERSION 3.24)
project(liminal VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(LIMINAL_WITH_INFERENCE "Build with llama.cpp local inference" ON)
option(LIMINAL_BUILD_EDITOR   "Build the editor (pulls ImGuizmo)" ${PROJECT_IS_TOP_LEVEL})
option(LIMINAL_BUILD_PLAYER   "Build standalone game player" ${PROJECT_IS_TOP_LEVEL})
option(LIMINAL_BUILD_TESTS    "Build headless tests (ctest)" ${PROJECT_IS_TOP_LEVEL})

# Warning flags applied ONLY to liminal's own translation units — never to the
# vendored sources we compile in-tree (ImGui, ImGuizmo, ImGuiColorTextEdit,
# libvterm, stb, miniaudio, glad), which are noisy under -Wextra. Subdirectories
# (editor/player/tests) inherit this variable and scope it to their own files.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") # also matches AppleClang
    set(LIMINAL_WARNING_FLAGS -Wall -Wextra)
else()
    set(LIMINAL_WARNING_FLAGS "")
endif()

# ---------------------------------------------------------------------------
# Platform → OpenGL version selection.
# Apple froze OpenGL at 4.1 core; everywhere else we can ask for 4.6 core.
# The chosen version flows into both the glad loader generation and PUBLIC
# compile definitions consumers use when creating the GLFW context.
# ---------------------------------------------------------------------------
if(APPLE)
    set(LIMINAL_GL_MAJOR 4)
    set(LIMINAL_GL_MINOR 1)
else()
    set(LIMINAL_GL_MAJOR 4)
    set(LIMINAL_GL_MINOR 6)
endif()
message(STATUS "liminal: targeting OpenGL ${LIMINAL_GL_MAJOR}.${LIMINAL_GL_MINOR} core profile")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

# Short git commit baked into the version header (shown on the editor landing
# screen). Best-effort: a tarball/export checkout with no .git falls back to
# "unknown". Note this resolves at CONFIGURE time, so a rebuild after new commits
# without re-running CMake keeps the old hash — acceptable for a UI label.
find_package(Git QUIET)
set(LIMINAL_GIT_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE LIMINAL_GIT_COMMIT_OUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(LIMINAL_GIT_COMMIT_OUT)
        set(LIMINAL_GIT_COMMIT "${LIMINAL_GIT_COMMIT_OUT}")
    endif()
endif()

# Configured version header → <build>/include/liminal/version.hpp
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/liminal/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/liminal/version.hpp
    @ONLY)

# Newer macOS ld (Xcode 15+/ld-prime) warns when a static lib (glfw) reaches an
# executable's link line via more than one path — here liminal's PRIVATE glfw
# dep (propagated through the static archive) plus the editor/player's own
# glfw link for the GLFW_KEY_* constants. The duplication is harmless (same
# archive), so silence the warning when the active linker understands the flag.
include(CheckLinkerFlag)
check_linker_flag(CXX "-Wl,-no_warn_duplicate_libraries"
    LIMINAL_LD_NO_WARN_DUP_LIBS)
if(LIMINAL_LD_NO_WARN_DUP_LIBS)
    add_link_options("-Wl,-no_warn_duplicate_libraries")
endif()

# ---------------------------------------------------------------------------
# liminal_imgui: Dear ImGui compiled exactly once (core + GLFW + OpenGL3
# backends). The GL3 backend uses its own embedded loader header, so it
# coexists peacefully with glad.
# ---------------------------------------------------------------------------
add_library(liminal_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(liminal_imgui PUBLIC
    $<BUILD_INTERFACE:${imgui_SOURCE_DIR}>
    $<BUILD_INTERFACE:${imgui_SOURCE_DIR}/backends>
    $<INSTALL_INTERFACE:include/liminal-imgui>)
target_link_libraries(liminal_imgui PUBLIC glfw)
add_library(liminal::imgui ALIAS liminal_imgui)

# ImGui 1.92's font system calls into the FreeType atlas loader from its CORE
# translation units (imgui.cpp / imgui_draw.cpp) whenever IMGUI_ENABLE_FREETYPE
# is defined — so the freetype builder must live in THIS library, not just the
# editor executable, or the player/tests linking liminal_imgui hit undefined
# ImGuiFreeType symbols. Gate on LIMINAL_BUILD_EDITOR since freetype is fetched
# in that branch of cmake/Dependencies.cmake; the editor merges Noto Color Emoji
# with the LoadColor flag. The vendored imgui_freetype.cpp stays out of the
# -Wall/-Wextra scope (liminal_imgui carries no warning flags). The define +
# freetype link are PUBLIC so every consumer (editor/player/tests) is consistent.
if(LIMINAL_BUILD_EDITOR)
    target_sources(liminal_imgui PRIVATE
        ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp)
    # IMGUI_USE_WCHAR32 widens ImWchar to 32 bits so codepoints in Unicode
    # plane 1 (emoji live at U+1F300+) survive ImGui's UTF-8 decode instead of
    # collapsing to U+FFFD — required for the color-emoji glyphs to render. It
    # changes ImFontGlyph/ImWchar layout, so it MUST be PUBLIC + consistent
    # across every consumer of liminal_imgui (lib/editor/player/tests).
    target_compile_definitions(liminal_imgui PUBLIC
        IMGUI_ENABLE_FREETYPE IMGUI_USE_WCHAR32)
    target_link_libraries(liminal_imgui PUBLIC freetype)
endif()

# ---------------------------------------------------------------------------
# Built-in shader packs (retro + native), embedded as string literals at
# configure time so the Renderer's default ctor never touches the filesystem.
# The blit (upscale) pass is shared by both packs.
# ---------------------------------------------------------------------------
set(LIMINAL_RETRO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/retro)
set(LIMINAL_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/native)
file(READ ${LIMINAL_RETRO_DIR}/dream.vert LIMINAL_RETRO_SCENE_VERT)
file(READ ${LIMINAL_RETRO_DIR}/dream.frag LIMINAL_RETRO_SCENE_FRAG)
file(READ ${LIMINAL_RETRO_DIR}/blit.vert  LIMINAL_RETRO_BLIT_VERT)
file(READ ${LIMINAL_RETRO_DIR}/blit.frag  LIMINAL_RETRO_BLIT_FRAG)
file(READ ${LIMINAL_NATIVE_DIR}/scene.vert LIMINAL_NATIVE_SCENE_VERT)
file(READ ${LIMINAL_NATIVE_DIR}/scene.frag LIMINAL_NATIVE_SCENE_FRAG)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/embedded_shaders.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.hpp
    @ONLY)
# file(READ) leaves no configure dependency; declare one so editing a shader
# regenerates the embed without a manual re-configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${LIMINAL_RETRO_DIR}/dream.vert
    ${LIMINAL_RETRO_DIR}/dream.frag
    ${LIMINAL_RETRO_DIR}/blit.vert
    ${LIMINAL_RETRO_DIR}/blit.frag
    ${LIMINAL_NATIVE_DIR}/scene.vert
    ${LIMINAL_NATIVE_DIR}/scene.frag)

# ---------------------------------------------------------------------------
# liminal: the engine static library.
# ---------------------------------------------------------------------------
add_library(liminal STATIC
    src/core/app.cpp
    src/core/asset_cache.cpp
    src/core/assets.cpp
    src/core/pak.cpp
    src/core/platform.cpp
    src/core/pty.cpp
    src/core/window.cpp
    src/scene/scene.cpp
    src/scene/components.cpp
    src/scene/component_registry.cpp
    src/scene/serialize.cpp
    src/scene/raycast.cpp
    src/render/shader.cpp
    src/render/mesh.cpp
    src/render/texture.cpp
    src/render/renderer.cpp
    src/audio/audio.cpp
    src/procgen/tileset.cpp
    src/procgen/wfc.cpp
    src/procgen/terrain.cpp
    src/procgen/terrain_field.cpp
    src/procgen/layout_validator.cpp
    src/procgen/shape_grammar.cpp
    src/procgen/structure.cpp
    src/ui/imgui_layer.cpp)
add_library(liminal::liminal ALIAS liminal)

# liminal is 100% first-party sources — warn on all of them.
target_compile_options(liminal PRIVATE ${LIMINAL_WARNING_FLAGS})

target_include_directories(liminal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>)
# Generated embeds are an implementation detail — private.
target_include_directories(liminal PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_compile_definitions(liminal PUBLIC
    LIMINAL_GL_MAJOR=${LIMINAL_GL_MAJOR}
    LIMINAL_GL_MINOR=${LIMINAL_GL_MINOR})
if(APPLE)
    # Apple marked OpenGL deprecated in 10.14; it still works fine.
    target_compile_definitions(liminal PUBLIC GL_SILENCE_DEPRECATION)
endif()

target_link_libraries(liminal
    PUBLIC
        glm::glm
        liminal_imgui
        nlohmann_json::nlohmann_json
        EnTT::EnTT
    PRIVATE
        glfw
        glad_gl
        liminal_stb
        miniaudio)

# Scripting is non-optional: Lua 5.4 + sol2 always compile in.
# sol2 is header-only and appears in <liminal/script/script_host.hpp>;
# it needs the Lua headers/library alongside it.
target_sources(liminal PRIVATE
    src/script/script_host.cpp
    src/script/lua_bindings.cpp
    src/script/lua_bindings_procgen.cpp)
target_link_libraries(liminal PUBLIC sol2::sol2 lua_static)

if(LIMINAL_WITH_INFERENCE)
    # llama stays PRIVATE: <liminal/inference/engine.hpp> exposes no llama
    # types, so consumers need only liminal::liminal. The compile definition
    # is PUBLIC because the header refuses to compile without it.
    target_sources(liminal PRIVATE src/inference/engine.cpp)
    target_link_libraries(liminal PRIVATE llama)
    target_compile_definitions(liminal PUBLIC LIMINAL_WITH_INFERENCE=1)
endif()

if(LIMINAL_BUILD_EDITOR)
    add_subdirectory(editor)
endif()

if(LIMINAL_BUILD_PLAYER)
    # player is added LAST so its CMakeLists can see whether liminal-editor was
    # defined and attach the "copy next to the editor" POST_BUILD rule there
    # (add_custom_command(TARGET) must name a target from its own directory).
    add_subdirectory(player)
endif()

if(LIMINAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
