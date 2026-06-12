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
# opens something immediately.
target_compile_definitions(liminal-editor PRIVATE
    LIMINAL_EDITOR_SAMPLE_PROJECT="${CMAKE_CURRENT_SOURCE_DIR}/sample_project/project.ljson")
