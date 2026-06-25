# ---------------------------------------------------------------------------
# Liminal third-party dependencies, all via FetchContent.
# The GLFW/glad/GLM/stb/ImGui/json/miniaudio/llama declares are transplanted
# verbatim from Grey Matter (hard-won quirk comments included).
# ---------------------------------------------------------------------------
include(FetchContent)
# Re-runs of cmake should not re-download anything.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# --- GLFW (windowing / input) ----------------------------------------------
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

# --- glad2 (OpenGL function loader, generated at configure time) -----------
# The generator is a Python tool (needs jinja2); any Python interpreter with
# jinja2 installed works — pass -DPython_EXECUTABLE=/path/to/python if your
# default python3 lacks it. REPRODUCIBLE pins the Khronos spec snapshot that
# ships in the repo instead of fetching the latest one over the network.
FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad
    GIT_TAG        v2.0.8
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake)

# Fail early with a useful message if the chosen Python cannot import jinja2.
# We try the default python3 first; it may work if jinja2 is installed
# globally. Otherwise the user must point us at a suitable interpreter.
find_package(Python COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import jinja2"
    RESULT_VARIABLE LIMINAL_JINJA2_RC
    OUTPUT_QUIET ERROR_QUIET)
if(NOT LIMINAL_JINJA2_RC EQUAL 0)
    message(FATAL_ERROR
        "glad's loader generator needs a Python with the 'jinja2' package, "
        "but '${Python_EXECUTABLE}' cannot import it.\n"
        "Either `pip install jinja2` into that interpreter, or re-configure "
        "with -DPython_EXECUTABLE=/path/to/python (e.g. a venv) that has it.")
endif()

# --- GLM (math) -------------------------------------------------------------
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE)

# --- stb (stb_image.h) ------------------------------------------------------
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb
    GIT_SHALLOW    TRUE)

# --- Dear ImGui (docking branch; no upstream CMake, we build it ourselves) --
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG        v1.92.8-docking
    GIT_SHALLOW    TRUE)

# --- nlohmann/json -----------------------------------------------------------
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)

# --- miniaudio (audio) -------------------------------------------------------
set(MINIAUDIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio
    GIT_TAG        0.11.25
    GIT_SHALLOW    TRUE)

# --- EnTT (entity-component-system) ------------------------------------------
FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt
    GIT_TAG        v3.15.0
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(glfw glad glm stb imgui json miniaudio entt)

# --- llama.cpp (local inference), static — gated by LIMINAL_WITH_INFERENCE ---
if(LIMINAL_WITH_INFERENCE)
    # Minimal embedded build: just the core `llama` library (which already
    # contains the GBNF grammar engine). Metal + Accelerate default ON on Apple.
    set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp
        GIT_TAG        b9585
        GIT_SHALLOW    TRUE)

    # llama.cpp is C++17 code. Our top-level CMAKE_CXX_STANDARD=20 would leak
    # into its targets, and under C++20 MSVC types u8"" literals as char8_t,
    # which llama-chat.cpp does not compile against. Scope the standard down
    # for llama only (plain variable — read at target creation time).
    set(_liminal_saved_cxx_standard ${CMAKE_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD 17)
    FetchContent_MakeAvailable(llama)
    set(CMAKE_CXX_STANDARD ${_liminal_saved_cxx_standard})
    unset(_liminal_saved_cxx_standard)
endif()

# --- Lua 5.4 + sol2 (scripting) — always built, non-optional -----------------
# walterschell/Lua wraps the upstream Lua 5.4 sources in a tidy CMake
# build; exposes target `lua_static`.
set(LUA_BUILD_BINARY  OFF CACHE BOOL "" FORCE)
set(LUA_BUILD_COMPILER OFF CACHE BOOL "" FORCE)
set(LUA_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(LUA_ENABLE_SHARED  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/walterschell/Lua
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE)

set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2
    GIT_TAG        v3.5.0
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(lua sol2)

# --- ImGuizmo (gizmos) — sources only; compiled into liminal-editor ----------
if(LIMINAL_BUILD_EDITOR)
    # The last release tag (1.83, 2021) predates ImGui 1.92, so we pin a
    # master commit that builds against the 1.92 docking branch (HEAD as of
    # 2026-06). No GIT_SHALLOW: shallow fetch of a raw SHA is unreliable
    # across git versions. Upstream's own CMakeLists assumes a preinstalled
    # imgui it can't find here — the bogus SOURCE_SUBDIR skips
    # add_subdirectory entirely; liminal-editor compiles src/ImGuizmo.cpp
    # itself against liminal_imgui.
    FetchContent_Declare(imguizmo
        GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo
        GIT_TAG        be8aa4aeab86b402701c8c1df011bd8cd776760b
        SOURCE_SUBDIR  cmake-skip-upstream-build)
    FetchContent_MakeAvailable(imguizmo)

    # ImGuiColorTextEdit — sources only; compiled into liminal-editor (same
    # treatment as ImGuizmo). We pin BalazsJako's original at a recent master
    # commit rather than the goossens fork: goossens is a full rewrite whose
    # API drops SetErrorMarkers / IsTextChanged / SetHandleKeyboardInputs /
    # GetCursorPosition+mCharAdvance — the exact surface the Script Editor pane
    # (diagnostics, completion popup key-stealing, cursor-anchored popup) is
    # built on. The pinned BalazsJako head already uses the modern ImGuiKey_*
    # API and compiles clean against imgui v1.92.8-docking (verified). No
    # GIT_SHALLOW with a raw SHA (see the ImGuizmo note above); upstream's
    # CMakeLists wants a preinstalled imgui, so the bogus SOURCE_SUBDIR skips
    # add_subdirectory and liminal-editor builds TextEditor.cpp itself.
    FetchContent_Declare(imguicolortextedit
        GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit
        GIT_TAG        ca2f9f1462e3b60e56351bc466acda448c5ea50d
        SOURCE_SUBDIR  cmake-skip-upstream-build)
    FetchContent_MakeAvailable(imguicolortextedit)

    # libvterm — the VT100/xterm engine behind the editor's terminal panel.
    # Neovim's mirror builds with a plain Makefile (no usable CMake), and its
    # core is pure C in src/*.c against include/vterm.h. Same treatment as
    # ImGuizmo/TextEditor: Populate the source, compile the .c files straight
    # into liminal-editor (see editor/CMakeLists.txt), and point at include/.
    # No GIT_SHALLOW with a raw SHA (see the ImGuizmo note); the bogus
    # SOURCE_SUBDIR skips any upstream add_subdirectory.
    FetchContent_Declare(libvterm
        GIT_REPOSITORY https://github.com/neovim/libvterm
        GIT_TAG        934bc2fbf21800ac3458a499df8820ca5fb45fd3
        SOURCE_SUBDIR  cmake-skip-upstream-build)
    FetchContent_MakeAvailable(libvterm)

    # cpp-httplib — header-only HTTP server behind the editor's MCP server (the
    # in-editor Model Context Protocol endpoint Claude Code introspects the live
    # scene through). Plaintext localhost only: we never enable
    # CPPHTTPLIB_OPENSSL_SUPPORT. Its CMake defines an INTERFACE target
    # `httplib::httplib` carrying the include dir + Threads dependency.
    #
    # REQUIRE_* alone is not enough: cpp-httplib also has USE_*_IF_AVAILABLE
    # knobs (default ON) that opportunistically detect OpenSSL/zlib/brotli and,
    # when found, enable the feature AND add that library's include dir to the
    # interface target. On a machine where those happen to resolve to a DIFFERENT
    # SDK than the active toolchain (e.g. CommandLineTools headers under an Xcode
    # libc++), the injected `-I .../usr/include` lands ahead of libc++'s own C
    # header shims and breaks <cmath>/<cstring> ("didn't find libc++'s <math.h>").
    # Force the opportunistic knobs off too so the MCP server stays plaintext and
    # pulls in zero external include dirs.
    set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_REQUIRE_ZLIB    OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_REQUIRE_BROTLI  OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
        GIT_TAG        v0.18.3
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(httplib)

    # JetBrains Mono for the editor UI font. Plain zip of TTFs (no CMake), so
    # we Populate-and-point like stb/glad — the editor bakes the absolute path
    # to the Regular face and loads it at startup.
    FetchContent_Declare(jetbrains_mono
        URL https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip)
    FetchContent_MakeAvailable(jetbrains_mono)
    set(LIMINAL_JETBRAINS_MONO_TTF
        "${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf")
endif()

# glad: generate a static loader for exactly the GL version we target.
glad_add_library(glad_gl STATIC REPRODUCIBLE API gl:core=${LIMINAL_GL_MAJOR}.${LIMINAL_GL_MINOR})

# stb is header-only; expose its directory.
add_library(liminal_stb INTERFACE)
target_include_directories(liminal_stb INTERFACE ${stb_SOURCE_DIR})
