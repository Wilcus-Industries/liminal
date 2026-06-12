add_executable(01_window main.cpp)
# glad/glfw linked directly: the example calls glClear and uses GLFW key
# codes itself (liminal links them PRIVATE).
target_link_libraries(01_window PRIVATE liminal::liminal glfw glad_gl)
