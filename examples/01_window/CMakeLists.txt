add_executable(01_window main.cpp)
# Raw GLFW/glad for now — the engine Window class arrives in Phase 1, at
# which point examples will stop linking these directly.
target_link_libraries(01_window PRIVATE liminal::liminal glfw glad_gl)
