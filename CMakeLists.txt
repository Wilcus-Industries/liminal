cmake_minimum_required(VERSION 3.24)
project(liminal VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(LIMINAL_WITH_INFERENCE "Build with llama.cpp local inference" ON)
option(LIMINAL_WITH_SCRIPTING "Build with Lua 5.4 + sol2 scripting" ON)
option(LIMINAL_BUILD_EDITOR   "Build the editor (pulls ImGuizmo)" ${PROJECT_IS_TOP_LEVEL})
option(LIMINAL_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})

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

# Configured version header → <build>/include/liminal/version.hpp
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/liminal/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/liminal/version.hpp
    @ONLY)

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
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(liminal_imgui PUBLIC glfw)
add_library(liminal::imgui ALIAS liminal_imgui)

# ---------------------------------------------------------------------------
# Default "retro" shader pack, embedded as string literals at configure time
# so the Renderer's default ctor never touches the filesystem.
# ---------------------------------------------------------------------------
set(LIMINAL_RETRO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/retro)
file(READ ${LIMINAL_RETRO_DIR}/dream.vert LIMINAL_RETRO_SCENE_VERT)
file(READ ${LIMINAL_RETRO_DIR}/dream.frag LIMINAL_RETRO_SCENE_FRAG)
file(READ ${LIMINAL_RETRO_DIR}/blit.vert  LIMINAL_RETRO_BLIT_VERT)
file(READ ${LIMINAL_RETRO_DIR}/blit.frag  LIMINAL_RETRO_BLIT_FRAG)
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
    ${LIMINAL_RETRO_DIR}/blit.frag)

# ---------------------------------------------------------------------------
# liminal: the engine static library.
# ---------------------------------------------------------------------------
add_library(liminal STATIC
    src/version.cpp
    src/core/assets.cpp
    src/core/window.cpp
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

target_include_directories(liminal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include)
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

if(LIMINAL_WITH_SCRIPTING)
    # sol2 is header-only and appears in public engine headers eventually;
    # it needs the Lua headers/library alongside it.
    target_link_libraries(liminal PUBLIC sol2::sol2 lua_static)
    target_compile_definitions(liminal PUBLIC LIMINAL_WITH_SCRIPTING=1)
endif()

if(LIMINAL_WITH_INFERENCE)
    # llama stays PRIVATE: <liminal/inference/engine.hpp> exposes no llama
    # types, so consumers need only liminal::liminal. The compile definition
    # is PUBLIC because the header refuses to compile without it.
    target_sources(liminal PRIVATE src/inference/engine.cpp)
    target_link_libraries(liminal PRIVATE llama)
    target_compile_definitions(liminal PUBLIC LIMINAL_WITH_INFERENCE=1)
endif()

if(LIMINAL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
