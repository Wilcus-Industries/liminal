# liminal-player: standalone runtime that plays a game whose project files are
# baked into a pak appended to this binary (or a sidecar "<exe>.pak"). Scripting
# is non-optional — a shipped game without Lua behavior is pointless.
add_executable(liminal-player main.cpp)
target_compile_options(liminal-player PRIVATE ${LIMINAL_WARNING_FLAGS})
# glfw linked for GLFW_KEY_* constants (ESC-to-quit) only.
target_link_libraries(liminal-player PRIVATE liminal::liminal glfw)

# When the editor is also being built, drop the player binary next to it so the
# editor (chunk 7) can find it to build/export games. This subdirectory is added
# last (see root CMakeLists), so liminal-editor is already defined here, and the
# custom command attaches to liminal-player — a target local to this directory.
if(TARGET liminal-editor)
    add_dependencies(liminal-editor liminal-player)
    add_custom_command(TARGET liminal-player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:liminal-player>
            $<TARGET_FILE_DIR:liminal-editor>/$<TARGET_FILE_NAME:liminal-player>
        COMMENT "Copying liminal-player next to liminal-editor")
endif()
