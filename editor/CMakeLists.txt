# liminal-editor: dockable ImGui scene editor. ImGuizmo is compiled straight
# into the executable (no upstream build system); its sources come from the
# FetchContent populate in cmake/Dependencies.cmake.
add_executable(liminal-editor
    main.cpp
    editor_app.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)

target_include_directories(liminal-editor PRIVATE ${imguizmo_SOURCE_DIR}/src)

# glfw for GLFW_KEY_* constants; liminal_imgui already carries the ImGui
# include dirs (incl. imgui_internal.h for the DockBuilder API).
target_link_libraries(liminal-editor PRIVATE liminal::liminal liminal::imgui glfw)

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

# Bakes the JetBrains Mono path fetched in cmake/Dependencies.cmake; the editor
# loads it as its default ImGui font (existence is re-checked at runtime).
if(LIMINAL_JETBRAINS_MONO_TTF)
    target_compile_definitions(liminal-editor PRIVATE
        LIMINAL_EDITOR_FONT_TTF="${LIMINAL_JETBRAINS_MONO_TTF}")
endif()
