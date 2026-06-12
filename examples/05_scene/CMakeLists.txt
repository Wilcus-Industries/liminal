add_executable(05_scene main.cpp)
# glfw linked for GLFW_KEY_* constants only.
target_link_libraries(05_scene PRIVATE liminal::liminal glfw)
